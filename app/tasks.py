from app.celery_app import celery_app
from app.services.account_service import execute_account_fetcher
from app.models.trade import Credentials
import logging
import json
import os
from datetime import datetime
import asyncio
import rapi

logger = logging.getLogger(__name__)


class OrderData:
    def __init__(
        self,
        order_id,
        account_id,
        symbol,
        exchange,
        side,
        order_type,
        status,
        quantity,
        filled_quantity,
        price,
        commission,
        timestamp,
    ):
        self.order_id = order_id
        self.account_id = account_id
        self.symbol = symbol
        self.exchange = exchange
        self.side = side
        self.order_type = order_type
        self.status = status
        self.quantity = quantity
        self.filled_quantity = filled_quantity
        self.price = price
        self.commission = commission
        self.timestamp = timestamp

    def to_dict(self):
        return {
            "order_id": self.order_id,
            "account_id": self.account_id,
            "symbol": self.symbol,
            "exchange": self.exchange,
            "side": self.side,
            "order_type": self.order_type,
            "status": self.status,
            "quantity": self.quantity,
            "filled_quantity": self.filled_quantity,
            "price": self.price,
            "commission": self.commission,
            "timestamp": self.timestamp,
        }


@celery_app.task(bind=True)
def fetch_accounts(self, credentials: dict):
    """
    Celery task to fetch accounts using REngine
    """
    try:
        # Convert dict to Credentials model
        creds = Credentials(**credentials)

        # Execute account fetcher
        success, message, accounts = execute_account_fetcher(creds)

        if not success:
            logger.error(f"Failed to fetch accounts: {message}")
            return {"success": False, "message": message, "accounts": []}

        # Convert accounts to dict for serialization
        accounts_dict = [
            {"account_id": acc.account_id, "fcm_id": acc.fcm_id, "ib_id": acc.ib_id}
            for acc in accounts
        ]

        return {"success": True, "message": message, "accounts": accounts_dict}

    except Exception as e:
        logger.error(f"Error in fetch_accounts task: {str(e)}")
        return {"success": False, "message": str(e), "accounts": []}


@celery_app.task(bind=True)
async def fetch_orders(self, credentials: dict, account_ids: list, start_date: str):
    """
    Celery task to fetch orders for specified accounts
    """
    engine = None
    try:
        # Load connection parameters
        connection_params = load_connection_params(
            credentials["server_type"], credentials["location"]
        )
        logger.info("Successfully loaded connection parameters")

        # Create REngine instance
        engine = rapi.REngine(
            "DeltalytixRithmicAPI",
            "1.0.0.0",
            connection_params,
            credentials["server_type"],
            credentials["location"],
        )
        logger.info("Successfully created REngine instance")

        # Set up callbacks and state
        orders_by_account = {}
        commission_rates = {}
        login_completed = asyncio.Event()
        account_list_received = asyncio.Event()
        order_replay_completed = asyncio.Event()

        def on_account_list(account_list):
            """Callback for when account list is received"""
            logger.info(f"Received account list with {len(account_list)} accounts")
            account_list_received.set()

        def on_product_rms_list(product_rms_list):
            """Callback for RMS product list"""
            for rms_info in product_rms_list:
                product_code = rms_info.product_code
                commission_rates[product_code] = {
                    "rate": (
                        rms_info.commission_fill_rate
                        if rms_info.commission_fill_rate
                        else 0.0
                    ),
                    "is_valid": bool(rms_info.commission_fill_rate),
                }
            logger.info(f"Received RMS info for {len(product_rms_list)} products")

        def on_order_replay(order_replay_info):
            """Callback for order replay"""
            if not order_replay_info or not order_replay_info.line_info_array:
                return

            for line_info in order_replay_info.line_info_array:
                if line_info.filled <= 0:
                    continue

                # Create order data
                order = OrderData(
                    order_id=line_info.order_num,
                    account_id=line_info.account.account_id,
                    symbol=line_info.ticker,
                    exchange=line_info.exchange,
                    side=line_info.buy_sell_type,
                    order_type=line_info.order_type,
                    status=line_info.status,
                    quantity=line_info.quantity_to_fill,
                    filled_quantity=line_info.filled,
                    price=line_info.avg_fill_price,
                    commission=0.0,  # Will be calculated later
                    timestamp=line_info.ssboe,
                )

                # Calculate commission
                product_code = (
                    order.symbol[:-2] if len(order.symbol) > 2 else order.symbol
                )
                if (
                    product_code in commission_rates
                    and commission_rates[product_code]["is_valid"]
                ):
                    commission_rate = commission_rates[product_code]["rate"]
                    order.commission = order.filled_quantity * commission_rate

                # Store order
                if order.account_id not in orders_by_account:
                    orders_by_account[order.account_id] = []
                orders_by_account[order.account_id].append(order)

            order_replay_completed.set()

        def on_alert(alert_type, message):
            """Callback for alerts"""
            logger.info(f"Alert {alert_type}: {message}")
            if alert_type == rapi.ALERT_LOGIN_COMPLETE:
                login_completed.set()

        # Set up callbacks
        engine.set_callbacks(
            on_account_list,
            on_order_replay,
            None,  # on_order_history_dates
            on_product_rms_list,
            on_alert,
        )

        # Login to the system
        if not engine.login(credentials["username"], credentials["password"]):
            error_code = engine.get_error_code()
            error_message = rapi.REngine.get_error_string(error_code)
            return {
                "success": False,
                "message": f"Failed to initiate login: {error_message}",
            }

        # Wait for login completion
        await login_completed.wait()

        # Process each account
        for account_id in account_ids:
            # Get RMS info for the account
            account = {"account_id": account_id}
            if not engine.get_product_rms_info(account):
                logger.error(f"Failed to get RMS info for account {account_id}")
                continue

            # Subscribe to orders
            if not engine.subscribe_order(account):
                logger.error(f"Failed to subscribe to orders for account {account_id}")
                continue

            # Get current session orders
            if not engine.replay_all_orders(account, 0, 0):
                logger.error(
                    f"Failed to get current session orders for account {account_id}"
                )
                continue

            # Wait for order replay completion
            await order_replay_completed.wait()

            # Get historical orders
            if not engine.list_order_history_dates(account):
                logger.error(f"Failed to get history dates for account {account_id}")
                continue

            # Process each historical date
            for date in engine.get_history_dates():
                if date >= start_date:
                    if not engine.replay_historical_orders(account, date):
                        logger.error(f"Failed to get historical orders for date {date}")
                        continue
                    await order_replay_completed.wait()

        # Convert orders to JSON format
        orders_json = {
            account_id: [order.to_dict() for order in orders]
            for account_id, orders in orders_by_account.items()
        }

        # Add metadata
        orders_json["status"] = "complete"
        orders_json["timestamp"] = int(datetime.now().timestamp())

        # Save to file
        filename = f"orders/orders_{int(datetime.now().timestamp())}.json"
        os.makedirs("orders", exist_ok=True)
        with open(filename, "w") as f:
            json.dump(orders_json, f, indent=2)

        return {
            "success": True,
            "message": f"Successfully retrieved orders for {len(account_ids)} accounts",
            "orders_file": filename,
        }

    except Exception as e:
        logger.error(f"Error in fetch_orders task: {str(e)}")
        return {"success": False, "message": str(e)}
    finally:
        if engine:
            try:
                engine.logout()
            except Exception as e:
                logger.error(f"Error during logout: {e}")
