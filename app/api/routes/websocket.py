from fastapi import APIRouter, WebSocket, WebSocketDisconnect, HTTPException
import logging
from app.core.security import verify_token
from app.services.websocket_service import ws_manager
from app.services.order_service import process_websocket_data
from starlette.websockets import WebSocketState
import asyncio
from starlette.datastructures import Headers
from websockets.exceptions import SecurityError, InvalidMessage
from starlette.websockets import WebSocket as StarletteWebSocket
from starlette.types import ASGIApp, Receive, Send, Scope

logger = logging.getLogger(__name__)
router = APIRouter()


class LoggingWebSocket(StarletteWebSocket):
    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        try:
            # Log the raw request data
            logger.info(f"Raw WebSocket request scope: {scope}")
            if "headers" in scope:
                logger.info(f"Raw headers: {scope['headers']}")

            # Call the parent class implementation
            await super().__call__(scope, receive, send)
        except Exception as e:
            logger.error(f"Error in WebSocket connection: {str(e)}", exc_info=True)
            raise


@router.websocket("/{session_id}")
async def websocket_endpoint(websocket: LoggingWebSocket, session_id: str):
    """WebSocket endpoint for real-time communication"""
    try:
        # Log the raw connection attempt
        logger.info(f"New WebSocket connection attempt for session {session_id}")

        # Add connection to manager (this will handle the accept)
        try:
            await ws_manager.connect(websocket, session_id)
            logger.info(
                f"WebSocket connection established and managed for session {session_id}"
            )
        except SecurityError as e:
            logger.error(f"Security error during WebSocket handshake: {str(e)}")
            await websocket.close(code=4000, reason="Security error during handshake")
            return
        except InvalidMessage as e:
            logger.error(f"Invalid message during WebSocket handshake: {str(e)}")
            await websocket.close(code=4000, reason="Invalid message during handshake")
            return
        except Exception as e:
            logger.error(f"Failed to establish WebSocket connection: {str(e)}")
            await websocket.close(code=4000, reason="Failed to establish connection")
            return

        # Now we can safely log headers after successful connection
        try:
            headers = dict(websocket.headers)
            logger.debug(f"Connection headers: {headers}")
        except Exception as e:
            logger.warning(f"Could not log headers: {str(e)}")

        while True:
            try:
                data = await asyncio.wait_for(websocket.receive_json(), timeout=30)
                logger.debug(
                    f"Received WebSocket data for session {session_id}: {data}"
                )

                if data.get("type") == "init":
                    # Verify token
                    token = data.get("token")
                    if not token:
                        logger.error(f"No token provided for session {session_id}")
                        await websocket.close(code=4001, reason="No token provided")
                        return

                    logger.debug(f"Verifying token for session {session_id}")
                    try:
                        payload = verify_token(token, session_id)
                        logger.info(f"Token verified for session {session_id}")
                    except Exception as e:
                        logger.error(f"Token verification failed: {str(e)}")
                        await websocket.close(code=4001, reason="Invalid token")
                        return

                    # Add userId from token to the data
                    data["userId"] = payload["user_id"]
                    logger.debug(f"Added userId to data: {data['userId']}")

                    # Get selected accounts and ensure it's a list
                    selected_accounts = data.get("accounts", [])
                    if not isinstance(selected_accounts, list):
                        selected_accounts = [selected_accounts]

                    if not selected_accounts:
                        logger.error(f"No accounts selected for session {session_id}")
                        await websocket.close(code=4002, reason="No accounts selected")
                        return

                    logger.info(
                        f"Starting process with accounts: {selected_accounts} for session {session_id}"
                    )

                    # Get session state
                    state = ws_manager.get_session_state(session_id)
                    if not state:
                        logger.error(f"No session state found for session {session_id}")
                        await websocket.close(
                            code=4003, reason="No session state found"
                        )
                        return

                    # Update session state
                    ws_manager.update_session_state(
                        session_id, status="running", accounts=selected_accounts
                    )

                    # Start processing
                    await process_websocket_data(session_id, selected_accounts, data)
                else:
                    # For other message types, broadcast to all clients in the session
                    await ws_manager.broadcast_to_session(session_id, data)

            except asyncio.TimeoutError:
                logger.warning(f"WebSocket receive timeout for session {session_id}")
                if websocket.client_state == WebSocketState.DISCONNECTED:
                    break
                continue
            except WebSocketDisconnect:
                logger.info(f"Client disconnected from session {session_id}")
                break
            except Exception as e:
                logger.error(
                    f"Error processing message for session {session_id}: {str(e)}",
                    exc_info=True,
                )
                await ws_manager.broadcast_log(session_id, str(e), level="error")

    except WebSocketDisconnect:
        logger.info(f"Client disconnected during handshake: {session_id}")
    except Exception as e:
        logger.error(
            f"WebSocket error for session {session_id}: {str(e)}", exc_info=True
        )
        try:
            await websocket.close(code=4000, reason=str(e))
        except:
            pass
    finally:
        logger.info(f"Cleaning up connection for session {session_id}")
        await ws_manager.disconnect(websocket, session_id)
