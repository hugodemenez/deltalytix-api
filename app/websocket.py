import json
import logging
from fastapi import WebSocket
from app.tasks import fetch_accounts, fetch_orders
from app.celery_app import celery_app
import asyncio

logger = logging.getLogger(__name__)


class WebSocketManager:
    def __init__(self):
        self.active_connections: dict[str, WebSocket] = {}
        self.task_states: dict[str, dict] = {}

    async def connect(self, websocket: WebSocket, client_id: str):
        await websocket.accept()
        self.active_connections[client_id] = websocket
        logger.info(f"Client {client_id} connected")

    def disconnect(self, client_id: str):
        if client_id in self.active_connections:
            del self.active_connections[client_id]
        if client_id in self.task_states:
            del self.task_states[client_id]
        logger.info(f"Client {client_id} disconnected")

    async def send_message(self, client_id: str, message: dict):
        if client_id in self.active_connections:
            await self.active_connections[client_id].send_json(message)

    async def handle_credentials(self, client_id: str, credentials: dict):
        try:
            # Start Celery task
            task = fetch_accounts.delay(credentials)

            # Store task ID
            self.task_states[client_id] = {"task_id": task.id, "status": "PENDING"}

            # Send initial status
            await self.send_message(
                client_id,
                {
                    "type": "task_status",
                    "status": "PENDING",
                    "message": "Account retrieval started",
                    "task_id": task.id,
                },
            )

            # Start monitoring task
            self.monitor_task(client_id, task.id)

        except Exception as e:
            logger.error(f"Error handling credentials for client {client_id}: {str(e)}")
            await self.send_message(client_id, {"type": "error", "message": str(e)})

    async def handle_order_request(self, client_id: str, data: dict):
        try:
            credentials = data.get("credentials", {})
            account_ids = data.get("account_ids", [])
            start_date = data.get("start_date")

            if not all([credentials, account_ids, start_date]):
                raise ValueError(
                    "Missing required fields: credentials, account_ids, or start_date"
                )

            # Start Celery task
            task = fetch_orders.delay(credentials, account_ids, start_date)

            # Store task ID
            self.task_states[client_id] = {"task_id": task.id, "status": "PENDING"}

            # Send initial status
            await self.send_message(
                client_id,
                {
                    "type": "task_status",
                    "status": "PENDING",
                    "message": "Order retrieval started",
                    "task_id": task.id,
                },
            )

            # Start monitoring task
            self.monitor_task(client_id, task.id)

        except Exception as e:
            logger.error(
                f"Error handling order request for client {client_id}: {str(e)}"
            )
            await self.send_message(client_id, {"type": "error", "message": str(e)})

    async def monitor_task(self, client_id: str, task_id: str):
        """Monitor Celery task and send updates to client"""
        try:
            while True:
                task = celery_app.AsyncResult(task_id)

                if task.ready():
                    result = task.get()
                    await self.send_message(
                        client_id,
                        {
                            "type": "task_complete",
                            "status": "COMPLETED",
                            "result": result,
                        },
                    )
                    break

                # Send progress update
                await self.send_message(
                    client_id,
                    {
                        "type": "task_status",
                        "status": task.status,
                        "message": "Processing...",
                    },
                )

                # Wait before next check
                await asyncio.sleep(1)

        except Exception as e:
            logger.error(f"Error monitoring task {task_id}: {str(e)}")
            await self.send_message(
                client_id,
                {"type": "error", "message": f"Task monitoring error: {str(e)}"},
            )


# Create global WebSocket manager instance
websocket_manager = WebSocketManager()
