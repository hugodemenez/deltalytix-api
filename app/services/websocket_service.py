from fastapi import WebSocket, WebSocketDisconnect
from typing import Set, Dict, Optional, List
import logging
from datetime import datetime
from app.models.websocket import WebSocketState, WebSocketMessage
from app.models.trade import OrderRequest
from app.services.trade_service import process_orders, store_trades
import jwt
import os
import json
from pydantic import BaseModel

logger = logging.getLogger(__name__)

# JWT utilities
SECRET_KEY = os.getenv("JWT_SECRET_KEY", "your-secret-key")


def decode_session_token(token: str) -> dict:
    try:
        return jwt.decode(token, SECRET_KEY, algorithms=["HS256"])
    except jwt.InvalidTokenError:
        raise WebSocketDisconnect(code=4001)


class WebSocketManager:
    def __init__(self):
        self.sessions: Dict[str, Set[WebSocket]] = {}
        self.states: Dict[str, WebSocketState] = {}
        self.messages: Dict[str, List[dict]] = {}  # Cache for messages
        self.orders_cache: Dict[str, Dict[str, dict]] = (
            {}
        )  # session_id -> order_id -> order_data
        self.last_connection_times: Dict[str, datetime] = (
            {}
        )  # Track last connection time per session
        self.connection_cooldown = 1800  # 30 minutes in seconds

    async def connect(self, websocket: WebSocket, session_id: str):
        """Connect a WebSocket client to a session"""
        try:
            # Check rate limit
            current_time = datetime.now()
            if session_id in self.last_connection_times:
                time_since_last_connection = (
                    current_time - self.last_connection_times[session_id]
                ).total_seconds()
                if time_since_last_connection < self.connection_cooldown:
                    remaining_time = int(
                        self.connection_cooldown - time_since_last_connection
                    )
                    logger.warning(
                        f"Rate limit exceeded for session {session_id}. Please wait {remaining_time} seconds."
                    )
                    raise WebSocketDisconnect(
                        code=4004,
                        reason=f"Rate limit exceeded. Please wait {remaining_time} seconds.",
                    )

            # Check header size
            headers = dict(websocket.headers)
            header_size = sum(
                len(f"{k}: {v}".encode("utf-8")) for k, v in headers.items()
            )
            if header_size > self.max_header_size:
                logger.error(
                    f"Header size ({header_size} bytes) exceeds limit ({self.max_header_size} bytes)"
                )
                raise WebSocketDisconnect(code=4000, reason="Headers too large")

            # Accept the connection first
            await websocket.accept()

            # Update last connection time
            self.last_connection_times[session_id] = current_time

            if session_id not in self.sessions:
                self.sessions[session_id] = set()
                self.messages[session_id] = []
                self.orders_cache[session_id] = {}

            self.sessions[session_id].add(websocket)
            logger.info(f"Client connected to session {session_id}")

            # Send cached messages and orders
            if self.messages[session_id]:
                for message in self.messages[session_id]:
                    try:
                        message_size = len(str(message).encode("utf-8"))
                        if message_size > self.max_message_size:
                            logger.warning(
                                f"Message size ({message_size} bytes) exceeds limit ({self.max_message_size} bytes)"
                            )
                            continue
                        await websocket.send_json(message)
                    except Exception as e:
                        logger.error(f"Error sending cached message: {e}")

            if self.orders_cache[session_id]:
                try:
                    orders_message = {
                        "type": "initial_data",
                        "orders": list(self.orders_cache[session_id].values()),
                    }
                    message_size = len(str(orders_message).encode("utf-8"))
                    if message_size <= self.max_message_size:
                        await websocket.send_json(orders_message)
                    else:
                        logger.warning(
                            f"Orders message size ({message_size} bytes) exceeds limit"
                        )
                except Exception as e:
                    logger.error(f"Error sending initial orders data: {e}")

        except Exception as e:
            logger.error(f"Error in WebSocket connection: {e}")
            await self.disconnect(websocket, session_id)
            raise

    async def disconnect(self, websocket: WebSocket, session_id: str):
        """Disconnect a WebSocket client from a session"""
        if session_id in self.sessions:
            self.sessions[session_id].remove(websocket)
            if not self.sessions[session_id]:
                # Clean up session data when last client disconnects
                del self.sessions[session_id]
                if session_id in self.states:
                    del self.states[session_id]
                if session_id in self.messages:
                    del self.messages[session_id]
                if session_id in self.orders_cache:
                    del self.orders_cache[session_id]
                # Don't delete last_connection_times as we want to maintain the rate limit
            logger.info(f"Client disconnected from session {session_id}")

    async def broadcast_to_session(self, session_id: str, message: dict):
        """Broadcast a message to all clients in a session"""
        if session_id not in self.sessions:
            return

        # Check message size
        message_size = len(str(message).encode("utf-8"))
        if message_size > self.max_message_size:
            logger.warning(
                f"Message size ({message_size} bytes) exceeds limit ({self.max_message_size} bytes)"
            )
            message = {
                "type": "error",
                "message": "Message size exceeds limit",
                "size": message_size,
                "limit": self.max_message_size,
            }

        # Cache message if it's not a temporary status update
        if message.get("type") not in ["status", "log"]:
            self.messages[session_id].append(message)

        # Cache orders if it's an order update
        if message.get("type") == "order_update" and "order" in message:
            order_data = message["order"]
            if "order_id" in order_data:
                self.orders_cache[session_id][order_data["order_id"]] = order_data

        disconnected = set()
        for connection in self.sessions[session_id]:
            try:
                await connection.send_json(message)
            except Exception as e:
                logger.error(f"Error broadcasting to client: {e}")
                disconnected.add(connection)

        # Clean up disconnected clients
        for conn in disconnected:
            await self.disconnect(conn, session_id)

    def update_session_state(self, session_id: str, **kwargs) -> None:
        """Update the state of a session"""
        if session_id not in self.states:
            self.states[session_id] = WebSocketState(
                session_id=session_id, status="initialized"
            )

        current_state = self.states[session_id]
        for key, value in kwargs.items():
            setattr(current_state, key, value)

    def get_session_state(self, session_id: str) -> Optional[WebSocketState]:
        """Get the state of a session"""
        return self.states.get(session_id)

    def get_messages(self, session_id: str) -> List[dict]:
        """Get cached messages for a session"""
        return self.messages.get(session_id, [])

    def get_orders(self, session_id: str) -> List[dict]:
        """Get cached orders for a session"""
        return list(self.orders_cache.get(session_id, {}).values())

    async def broadcast_status(
        self, session_id: str, message: str, status: str = "info"
    ):
        """Broadcast a status message to a session"""
        await self.broadcast_to_session(
            session_id, {"type": "status", "message": message, "status": status}
        )

    async def broadcast_log(self, session_id: str, message: str, level: str = "info"):
        """Broadcast a log message to a session"""
        await self.broadcast_to_session(
            session_id,
            {
                "type": "log",
                "message": message,
                "level": level,
                "timestamp": int(datetime.now().timestamp()),
            },
        )


# Create a global instance of WebSocketManager
ws_manager = WebSocketManager()


# Process Manager for handling messages
class ProcessManager:
    def __init__(self):
        self.processes: Dict[str, dict] = {}  # session_id -> process info
        self.messages: Dict[str, List[dict]] = {}  # session_id -> messages queue

    def get_messages(self, session_id: str) -> List[dict]:
        return self.messages.get(session_id, [])

    async def start_process(
        self,
        session_id: str,
        credentials,
        selected_accounts: List[str] = None,
    ):
        try:
            if not credentials.start_date:
                raise ValueError("start_date must be provided via WebSocket")

            logger.info(f"=== Starting process for session {session_id} ===")
            logger.info(f"Selected accounts: {selected_accounts}")

            # Check if process is already running
            if (
                session_id in self.processes
                and self.processes[session_id]["status"] == "running"
            ):
                logger.info(f"Process already running for session {session_id}")
                return

            selected_accounts = selected_accounts or []

            # Update process status to running
            self.messages[session_id] = []
            self.processes[session_id].update(
                {
                    "status": "running",
                    "started_at": datetime.now(),
                    "credentials": credentials,
                    "accounts": selected_accounts,
                }
            )

            # Start your process here...
            # This is a placeholder for your actual process starting logic
            try:
                # Create order request with credentials
                request = OrderRequest(
                    username=credentials.username,
                    password=credentials.password,
                    server_type=credentials.server_type,
                    server_name=credentials.location,
                    start_date=credentials.start_date,
                    account_ids=selected_accounts,
                    userId=credentials.userId,
                )

                # Log start of processing
                await ws_manager.broadcast_status(
                    session_id,
                    f"Starting order processing for {len(selected_accounts)} accounts",
                )

                # Execute order fetcher with session_id for real-time logging
                orders_data = await execute_order_fetcher(request, session_id)

                # Process orders and broadcast results
                await ws_manager.broadcast_to_session(
                    session_id,
                    {
                        "type": "orders",
                        "data": orders_data,
                        "message": f"Retrieved orders for {len(selected_accounts)} accounts",
                    },
                )

                # Process each order and broadcast updates
                for account_id, account_orders in orders_data.items():
                    if isinstance(account_orders, list):  # Skip metadata fields
                        for order in account_orders:
                            await ws_manager.broadcast_to_session(
                                session_id, {"type": "order_update", "order": order}
                            )

                # Process orders into trades
                await ws_manager.broadcast_status(
                    session_id,
                    "Processing orders into trades...",
                )
                trades, open_positions = process_orders(
                    orders_data,
                    credentials.userId,
                    None,  # Let process_orders fetch tick details
                )

                # Send trade processing results
                await ws_manager.broadcast_to_session(
                    session_id,
                    {
                        "type": "trades_processed",
                        "trades_count": len(trades),
                        "open_positions_count": len(open_positions),
                        "message": f"Processed {len(trades)} trades and found {len(open_positions)} open positions",
                    },
                )

                # Store trades in database
                if trades:
                    await ws_manager.broadcast_status(
                        session_id,
                        f"Storing {len(trades)} trades in database...",
                    )
                    await store_trades(trades, session_id)
                    await ws_manager.broadcast_status(
                        session_id,
                        f"Successfully stored {len(trades)} trades in database",
                        "success",
                    )

                # Update process status to completed
                self.processes[session_id]["status"] = "completed"
                self.processes[session_id]["completed_at"] = datetime.now()
                self.processes[session_id]["result"] = {
                    "trades_count": len(trades),
                    "open_positions_count": len(open_positions),
                    "message": "Processing completed successfully",
                }

                # Send completion status
                await ws_manager.broadcast_status(
                    session_id,
                    f"Successfully processed orders for {len(selected_accounts)} accounts",
                    "success",
                )

            except Exception as e:
                logger.error(f"Error in process execution: {e}")
                logger.exception(e)
                self.processes[session_id]["status"] = "error"
                self.processes[session_id]["error"] = str(e)
                await ws_manager.broadcast_log(
                    session_id, f"Error processing orders: {str(e)}", "error"
                )
                raise

        except Exception as e:
            logger.error(f"Failed to start process for session {session_id}: {e}")
            logger.exception(e)
            self.processes[session_id]["status"] = "error"
            raise


# Create a global instance of ProcessManager
process_manager = ProcessManager()


async def handle_websocket_connection(
    websocket: WebSocket, session_id: str, manager: WebSocketManager
):
    try:
        await manager.connect(websocket, session_id)
        while True:
            data = await websocket.receive_json()

            if data.get("type") == "init":
                try:
                    # Verify token
                    token = data.get("token")
                    payload = decode_session_token(token)
                    if payload["session_id"] != session_id:
                        await websocket.close(code=4001)
                        return

                    # Get selected accounts and ensure it's a list
                    selected_accounts = data.get("accounts", [])
                    if not isinstance(selected_accounts, list):
                        selected_accounts = [selected_accounts]

                    if not selected_accounts:
                        logger.error("No accounts selected")
                        await websocket.close(code=4002)
                        return

                    logger.info(f"Starting process with accounts: {selected_accounts}")

                    # Get stored credentials
                    process_info = process_manager.processes.get(session_id)
                    if not process_info:
                        logger.error("No process info found for session")
                        await websocket.close(code=4003)
                        return

                    # Check if process is already running
                    if process_info.get("status") == "running":
                        logger.info(f"Process already running for session {session_id}")
                        continue

                    credentials = process_info["credentials"]

                    # Update start_date from the WebSocket message if provided
                    start_date = data.get("start_date")
                    if start_date:
                        credentials.start_date = start_date

                    # Start processing for selected accounts
                    await process_manager.start_process(
                        session_id, credentials, selected_accounts
                    )
                except Exception as e:
                    logger.error(f"Error processing init message: {e}")
                    await websocket.close(code=4000)
                    return
            else:
                # Handle other message types
                await manager.broadcast_to_session(session_id, data)

    except WebSocketDisconnect:
        await manager.disconnect(websocket, session_id)
    except Exception as e:
        logger.error(f"Error in websocket connection: {e}")
        await manager.disconnect(websocket, session_id)
