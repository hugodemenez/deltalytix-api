from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
import logging
import os
from starlette.websockets import WebSocketDisconnect
from websockets.exceptions import WebSocketException

from app.core.config import settings, verify_environment
from app.utils.logging import setup_logging
from app.api.endpoints import router as api_router

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

    return app


# Create the application instance
app = create_application()
