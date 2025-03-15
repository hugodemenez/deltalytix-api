import logging
import asyncio
import json
import os
from typing import List
from app.models.trade import Credentials, AccountData
import rapi

logger = logging.getLogger(__name__)

# Reduce timeout since we'll fail fast on connection closed
LOGIN_TIMEOUT_SECONDS = 5


class LoginError(Exception):
    """Custom exception for login failures"""

    pass


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


async def execute_account_fetcher(
    credentials: Credentials,
) -> tuple[bool, str, List[AccountData]]:
    """Execute the account fetcher using the RApi package
    Returns:
        tuple: (success: bool, message: str, accounts: List[AccountData])
    """
    engine = None
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
        login_completed = asyncio.Event()
        connection_error = asyncio.Queue()  # Use a queue to pass error messages

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

            # Check if this is a Trading System alert
            is_trading_system = "Trading System" in message

            if alert_type == rapi.ALERT_LOGIN_FAILED and is_trading_system:
                connection_error.put_nowait(message)  # Immediately signal error
            elif alert_type == rapi.ALERT_LOGIN_COMPLETE and is_trading_system:
                login_completed.set()
            elif alert_type == rapi.ALERT_CONNECTION_CLOSED and is_trading_system:
                # Always handle connection closed for Trading System
                error_msg = "Trading System connection closed"
                connection_error.put_nowait(error_msg)  # Immediately signal error

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
            error_code = engine.get_error_code()
            error_message = rapi.REngine.get_error_string(error_code)
            return False, f"Failed to initiate login: {error_message}", []
        logger.info("Successfully initiated login")

        # Wait for either login completion, failure, or error
        try:
            # Create a task for login completion
            login_completed_task = asyncio.create_task(login_completed.wait())

            # Wait for login completion with timeout, but check for errors periodically
            start_time = asyncio.get_event_loop().time()
            while True:
                # Check if we have an error
                try:
                    error_message = connection_error.get_nowait()
                    login_completed_task.cancel()
                    try:
                        await login_completed_task
                    except asyncio.CancelledError:
                        pass
                    return False, error_message, []
                except asyncio.QueueEmpty:
                    pass

                # Check if login completed
                if login_completed.is_set():
                    break

                # Check if we've timed out
                if asyncio.get_event_loop().time() - start_time > LOGIN_TIMEOUT_SECONDS:
                    login_completed_task.cancel()
                    try:
                        await login_completed_task
                    except asyncio.CancelledError:
                        pass
                    return (
                        False,
                        f"Login timed out after {LOGIN_TIMEOUT_SECONDS} seconds",
                        [],
                    )

                # Small sleep to prevent busy waiting
                await asyncio.sleep(0.1)

            # If we get here, login completed successfully
            return True, "Login completed successfully", []

        except asyncio.TimeoutError:
            return False, f"Login timed out after {LOGIN_TIMEOUT_SECONDS} seconds", []

        # Request account list immediately after successful login
        if not engine.get_accounts():
            error_code = engine.get_error_code()
            error_message = rapi.REngine.get_error_string(error_code)
            return False, f"Failed to get accounts: {error_message}", []
        logger.info("Successfully requested account list")

        logger.info(f"Successfully retrieved {len(accounts)} accounts")
        return True, f"Successfully retrieved {len(accounts)} accounts", accounts

    except Exception as e:
        logger.error(f"Error executing account fetcher: {e}")
        logger.exception(e)
        return False, str(e), []
    finally:
        # Ensure we always try to logout and clean up
        if engine:
            try:
                if not engine.logout():
                    logger.warning("Failed to logout cleanly")
                else:
                    logger.info("Successfully logged out")
            except Exception as e:
                logger.error(f"Error during logout: {e}")
