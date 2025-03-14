import logging
from typing import Dict, List, Tuple
from datetime import datetime
from app.models.trade import ManualOrder, Trade, OpenPosition
from app.services.trade_service import process_orders, normalize_instrument

logger = logging.getLogger(__name__)


def convert_manual_orders_to_standard_format(
    orders: List[ManualOrder], user_id: str
) -> Dict:
    """Convert manual orders to the standard format expected by process_orders"""
    try:
        orders_by_account: Dict[str, List[Dict]] = {}

        for order in orders:
            if order.OrderState != "Filled":  # Only process filled orders
                continue

            # Convert to standard order format
            standard_order = {
                "order_id": order.OrderId,
                "account_id": order.AccountId,
                "symbol": order.Instrument.Symbol,
                "exchange": "MANUAL",  # Since we don't have exchange info
                "side": "B" if order.OrderAction.upper() == "BUY" else "S",
                "order_type": order.OrderType,
                "status": "FILLED",
                "quantity": order.Quantity,
                "filled_quantity": order.Quantity,  # Since we only process filled orders
                "price": order.AverageFilledPrice,
                "commission": 0.0,  # Set commission to 0 for manual orders
                "timestamp": int(order.Time.timestamp()),
            }

            # Group orders by account
            if order.AccountId not in orders_by_account:
                orders_by_account[order.AccountId] = []
            orders_by_account[order.AccountId].append(standard_order)

        return orders_by_account

    except Exception as e:
        logger.error(f"Error converting manual orders: {e}")
        logger.exception(e)
        raise


def process_manual_orders(
    orders: List[ManualOrder], user_id: str
) -> Tuple[List[Trade], List[OpenPosition]]:
    """Process manual orders into trades"""
    try:
        # Convert orders to standard format
        orders_data = convert_manual_orders_to_standard_format(orders, user_id)

        # Process orders using existing trade service
        trades, open_positions = process_orders(orders_data, user_id)

        return trades, open_positions

    except Exception as e:
        logger.error(f"Error processing manual orders: {e}")
        logger.exception(e)
        raise
