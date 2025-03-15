from fastapi import APIRouter, HTTPException
import logging
import json
import uuid
from app.core.security import create_session_token
from app.models.trade import Credentials, AccountListResponse
from app.services.websocket_service import ws_manager
from app.services.account_service import execute_account_fetcher

logger = logging.getLogger(__name__)
router = APIRouter()


@router.post("", response_model=AccountListResponse)
async def get_accounts(credentials: Credentials):
    """Get list of available trading accounts"""
    try:
        # Validate server configuration
        with open("server_configurations.json", "r") as f:
            config_data = json.load(f)

        if credentials.server_type not in config_data:
            raise HTTPException(
                status_code=400,
                detail=f"Invalid server type: {credentials.server_type}. Available types: {list(config_data.keys())}",
            )

        available_locations = config_data[credentials.server_type].get("locations", [])
        if credentials.location not in available_locations:
            raise HTTPException(
                status_code=400,
                detail=f"Invalid location for server type {credentials.server_type}. Available locations: {available_locations}",
            )

        # Execute account list fetcher
        success, message, accounts = await execute_account_fetcher(credentials)

        # If not successful, return error response
        if not success:
            return AccountListResponse(
                success=False,
                message=message,
                accounts=[],
                websocket_url="",
                token="",
            )

        # Create session
        session_id = str(uuid.uuid4())
        token = create_session_token(credentials.userId, session_id)

        # Store session state
        ws_manager.update_session_state(
            session_id, status="initialized", credentials=credentials.dict()
        )

        return AccountListResponse(
            success=True,
            message=message,
            accounts=accounts,
            websocket_url=f"ws://your-domain/ws/{session_id}",
            token=token,
        )

    except HTTPException as e:
        # Re-raise HTTP exceptions as they already have proper formatting
        raise
    except Exception as e:
        logger.error(f"Error retrieving accounts: {e}")
        logger.exception(e)
        # Return a properly formatted error response
        return AccountListResponse(
            success=False,
            message=str(e),
            accounts=[],
            websocket_url="",
            token="",
        )
