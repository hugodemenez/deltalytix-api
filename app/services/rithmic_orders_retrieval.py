import logging
import asyncio
import json
from typing import Dict, List, Optional
from datetime import datetime
from app.models.trade import OrderRequest
from app.services.websocket_service import ws_manager
from app.services.trade_service import process_orders, store_trades
import rapi  # Our Python bindings

logger = logging.getLogger(__name__)


class RithmicOrdersRetriever:
    def __init__(self):
        self.engine = None
        self.accounts = []
        self.commission_rates = {}
        self.orders_data = {}
        self.processing_stats = {}

    async def initialize(self, request: OrderRequest, session_id: str):
        """Initialize the Rithmic engine and set up callbacks"""
        try:
            # Create REngine instance
            self.engine = rapi.REngine("DeltalytixRithmicAPI", "1.0.0.0")

            # Set up callbacks
            self.engine.set_callbacks(
                on_account_list=self._handle_account_list,
                on_order_replay=self._handle_order_replay,
                on_order_history_dates=self._handle_order_history_dates,
                on_product_rms_list=self._handle_product_rms_list,
                on_alert=self._handle_alert,
            )

            # Login to Rithmic
            success = self.engine.login(
                request.username,
                request.password,
                "",  # No market data connection needed
                request.server_name,  # Trading system connection point
            )

            if not success:
                raise RuntimeError("Failed to login to Rithmic")

            # Wait for login completion
            while not self._login_complete:
                await asyncio.sleep(0.1)

            # Get accounts
            if not self.engine.get_accounts():
                raise RuntimeError("Failed to get accounts")

            # Wait for accounts
            while not self._accounts_received:
                await asyncio.sleep(0.1)

            # Filter accounts if specific ones were requested
            if request.account_ids:
                self.accounts = [
                    acc
                    for acc in self.accounts
                    if acc.account_id in request.account_ids
                ]

            if not self.accounts:
                raise ValueError("No valid accounts found")

            # Initialize processing stats for each account
            for account in self.accounts:
                self.processing_stats[account.account_id] = {
                    "total_days": 0,
                    "days_processed": 0,
                    "orders_processed": 0,
                }

            return True

        except Exception as e:
            logger.error(f"Error initializing RithmicOrdersRetriever: {e}")
            if session_id:
                await ws_manager.broadcast_log(
                    session_id,
                    f"Error initializing Rithmic connection: {str(e)}",
                    "error",
                )
            raise

    async def retrieve_orders(self, request: OrderRequest, session_id: str) -> Dict:
        """Retrieve orders for all accounts"""
        try:
            # Initialize if not already done
            if not self.engine:
                await self.initialize(request, session_id)

            # Process each account
            for account in self.accounts:
                account_id = account.account_id

                # Get RMS info for commission rates
                if not self.engine.get_product_rms_info(account):
                    logger.warning(f"Failed to get RMS info for account {account_id}")
                    continue

                # Get order history dates
                if not self.engine.list_order_history_dates(account):
                    logger.warning(
                        f"Failed to get history dates for account {account_id}"
                    )
                    continue

                # Wait for dates
                while not self._history_dates_received:
                    await asyncio.sleep(0.1)

                # Process current session orders first
                if not self.engine.replay_all_orders(account, 0, 0):
                    logger.warning(
                        f"Failed to get current orders for account {account_id}"
                    )
                    continue

                # Wait for orders
                while not self._orders_received:
                    await asyncio.sleep(0.1)

                # Process historical orders
                for date in self._history_dates:
                    if not self.engine.replay_historical_orders(account, date):
                        logger.warning(
                            f"Failed to get historical orders for date {date}"
                        )
                        continue

                    # Wait for orders
                    while not self._orders_received:
                        await asyncio.sleep(0.1)

                    # Update progress
                    self.processing_stats[account_id]["days_processed"] += 1
                    if session_id:
                        await ws_manager.broadcast_to_session(
                            session_id,
                            {
                                "type": "progress",
                                "account_id": account_id,
                                "days_processed": self.processing_stats[account_id][
                                    "days_processed"
                                ],
                                "total_days": self.processing_stats[account_id][
                                    "total_days"
                                ],
                                "orders_processed": self.processing_stats[account_id][
                                    "orders_processed"
                                ],
                            },
                        )

            # Process orders into trades
            if session_id:
                await ws_manager.broadcast_status(
                    session_id, "Processing orders into trades..."
                )

            trades, open_positions = process_orders(
                self.orders_data,
                request.userId,
                None,  # Let process_orders fetch tick details
            )

            # Store trades
            if trades:
                await store_trades(trades, session_id)

            # Send completion message
            if session_id:
                await ws_manager.broadcast_to_session(
                    session_id,
                    {
                        "type": "complete",
                        "trades_count": len(trades),
                        "open_positions_count": len(open_positions),
                        "message": f"Successfully processed {len(trades)} trades and found {len(open_positions)} open positions",
                    },
                )

            return self.orders_data

        except Exception as e:
            logger.error(f"Error retrieving orders: {e}")
            if session_id:
                await ws_manager.broadcast_log(
                    session_id, f"Error retrieving orders: {str(e)}", "error"
                )
            raise

    def _handle_account_list(self, accounts):
        """Handle account list callback"""
        self.accounts = accounts
        self._accounts_received = True

    def _handle_order_replay(self, orders):
        """Handle order replay callback"""
        for order in orders:
            account_id = order.account_id
            if account_id not in self.orders_data:
                self.orders_data[account_id] = []

            self.orders_data[account_id].append(
                {
                    "order_id": order.order_id,
                    "account_id": order.account_id,
                    "symbol": order.symbol,
                    "exchange": order.exchange,
                    "side": order.side,
                    "order_type": order.order_type,
                    "status": order.status,
                    "quantity": order.quantity,
                    "filled_quantity": order.filled_quantity,
                    "price": order.price,
                    "commission": order.commission,
                    "timestamp": order.timestamp,
                }
            )

            self.processing_stats[account_id]["orders_processed"] += 1
        self._orders_received = True

    def _handle_order_history_dates(self, dates):
        """Handle order history dates callback"""
        self._history_dates = dates
        self._history_dates_received = True

    def _handle_product_rms_list(self, commission_rates):
        """Handle product RMS list callback"""
        self.commission_rates = commission_rates

    def _handle_alert(self, alert_type, message):
        """Handle alert callback"""
        if alert_type == rapi.ALERT_LOGIN_COMPLETE:
            self._login_complete = True

    def cleanup(self):
        """Clean up resources"""
        if self.engine:
            self.engine.logout()
            self.engine = None
        self.accounts = []
        self.commission_rates = {}
        self.orders_data = {}
        self.processing_stats = {}
        self._login_complete = False
        self._accounts_received = False
        self._orders_received = False
        self._history_dates_received = False
        self._history_dates = []


# Create a global instance
rithmic_orders_retriever = RithmicOrdersRetriever()


async def retrieve_rithmic_orders(
    request: OrderRequest, session_id: str = None
) -> Dict:
    """Main function to retrieve Rithmic orders"""
    try:
        return await rithmic_orders_retriever.retrieve_orders(request, session_id)
    except Exception as e:
        logger.error(f"Error in retrieve_rithmic_orders: {e}")
        raise
    finally:
        rithmic_orders_retriever.cleanup()
