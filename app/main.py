from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
import logging
import os
from starlette.websockets import WebSocketDisconnect
from websockets.exceptions import WebSocketException
import uuid

from app.core.config import settings, verify_environment
from app.utils.logging import setup_logging
from app.api.endpoints import router as api_router
from app.websocket import websocket_manager

# Set up logging
setup_logging(
    level=getattr(logging, os.getenv("LOG_LEVEL", "INFO")),
    log_file=os.getenv("LOG_FILE"),
)

logger = logging.getLogger(__name__)


def create_application() -> FastAPI:
    """Create and configure the FastAPI application"""
    app = FastAPI(
        title=settings.PROJECT_NAME,
        openapi_url=f"{settings.API_V1_STR}/openapi.json",
        websocket_max_size=1024 * 1024,  # 1MB max message size
    )

    # Set up CORS with specific WebSocket settings
    app.add_middleware(
        CORSMiddleware,
        allow_origins=settings.BACKEND_CORS_ORIGINS,
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
        expose_headers=["*"],
    )

    # Include routers
    app.include_router(api_router, prefix=settings.API_V1_STR)

    @app.on_event("startup")
    async def startup_event():
        """Perform startup tasks"""
        try:
            # Verify environment variables
            verify_environment()
            logger.info("Environment verification completed successfully")
        except Exception as e:
            logger.error(f"Startup failed: {e}")
            raise

    @app.get("/health")
    async def health_check():
        """Health check endpoint"""
        return {"status": "healthy"}

    @app.websocket("/ws/{client_id}")
    async def websocket_endpoint(websocket: WebSocket, client_id: str):
        await websocket_manager.connect(websocket, client_id)
        try:
            while True:
                # Receive message from client
                data = await websocket.receive_json()

                # Handle different message types
                if data.get("type") == "credentials":
                    # Handle credentials and start account retrieval
                    await websocket_manager.handle_credentials(
                        client_id, data.get("credentials", {})
                    )
                elif data.get("type") == "get_orders":
                    # Handle order retrieval request
                    await websocket_manager.handle_order_request(client_id, data)

        except WebSocketDisconnect:
            websocket_manager.disconnect(client_id)
        except Exception as e:
            logger.error(f"WebSocket error: {str(e)}")
            websocket_manager.disconnect(client_id)

    return app


# Create the application instance
app = create_application()
