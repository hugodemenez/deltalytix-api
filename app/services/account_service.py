import logging
import asyncio
import json
import os
from typing import List
from app.models.trade import Credentials, AccountData
import rapi

logger = logging.getLogger(__name__)


def load_connection_params(server_type: str, location: str):
    """Load Rithmic connection parameters from configuration file"""
    try:
        with open("server_configurations.json", "r") as f:
            config = json.load(f)

            # Verify required sections exist
            if server_type not in config:
                raise ValueError(f"Missing '{server_type}' section in configuration")
            if "server_configs" not in config[server_type]:
                raise ValueError(
                    f"Missing 'server_configs' section in {server_type} configuration"
                )
            if location not in config[server_type]["server_configs"]:
                raise ValueError(f"Missing '{location}' configuration in {server_type}")

            # Log the configuration being used
            logger.info(f"Using {server_type}/{location} configuration")
            return json.dumps(config)
    except Exception as e:
        logger.error(f"Failed to load server configurations: {e}")
        raise RuntimeError(f"Failed to load server configurations: {e}")


async def execute_account_fetcher(credentials: Credentials) -> List[AccountData]:
    """Execute the account fetcher using the RApi package"""
    try:
        # Load connection parameters with server type and location
        connection_params = load_connection_params(
            credentials.server_type, credentials.location
        )
        logger.info("Successfully loaded connection parameters")

        # Create REngine with connection parameters
        engine = rapi.REngine(
            "DeltalytixRithmicAPI",
            "1.0.0.0",
            connection_params,
            credentials.server_type,
            credentials.location,
        )
        logger.info("Successfully created REngine instance")

        # Set up callbacks
        accounts = []

        def on_account_list(account_list):
            """Callback for when account list is received"""
            logger.info(f"Received account list with {len(account_list)} accounts")
            for acc in account_list:
                accounts.append(
                    AccountData(
                        account_id=acc.account_id, fcm_id=acc.fcm_id, ib_id=acc.ib_id
                    )
                )

        def on_alert(alert_type, message):
            """Callback for alerts"""
            logger.info(f"Alert {alert_type}: {message}")

        # Set up callbacks
        engine.set_callbacks(
            on_account_list,  # First callback
            None,  # on_order_replay
            None,  # on_order_history_dates
            None,  # on_product_rms_list
            on_alert,  # Last callback
        )
        logger.info("Successfully set up callbacks")

        # Login to the system
        if not engine.login(credentials.username, credentials.password):
            error_code = engine.get_error_code()  # Get the error code first
            error_message = rapi.REngine.get_error_string(
                error_code
            )  # Pass the error code
            raise RuntimeError(f"Failed to login: {error_message}")
        logger.info("Successfully logged in")

        # Request account list immediately after successful login
        if not engine.get_accounts():
            error_code = engine.get_error_code()
            error_message = rapi.REngine.get_error_string(error_code)
            raise RuntimeError(f"Failed to get accounts: {error_message}")
        logger.info("Successfully requested account list")

        # Wait for account list to be received (this is now handled by the C++ layer)
        # No need for sleep here as the C++ layer handles the waiting

        # Logout
        if not engine.logout():
            logger.warning("Failed to logout cleanly")
        else:
            logger.info("Successfully logged out")

        logger.info(f"Successfully retrieved {len(accounts)} accounts")
        return accounts

    except Exception as e:
        logger.error(f"Error executing account fetcher: {e}")
        logger.exception(e)
        raise
