from fastapi import APIRouter, HTTPException
import logging
import json
from app.models.trade import ServerResponse

logger = logging.getLogger(__name__)
router = APIRouter()


@router.get("", response_model=ServerResponse)
async def get_server_configurations():
    """Get available server configurations"""
    try:
        with open("server_configurations.json", "r") as f:
            config_data = json.load(f)

        servers = {
            server_type: data.get("locations", [])
            for server_type, data in config_data.items()
        }

        return ServerResponse(
            success=True,
            message="Successfully retrieved server configurations",
            servers=servers,
        )
    except Exception as e:
        logger.error(f"Error retrieving server configurations: {e}")
        raise HTTPException(status_code=500, detail=str(e))
