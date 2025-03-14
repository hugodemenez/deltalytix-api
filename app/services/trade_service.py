import logging
from typing import List, Tuple, Dict
from datetime import datetime, timezone
import uuid
from app.models.trade import Trade, QueuedOrder, OpenPosition
from app.db.session import db

logger = logging.getLogger(__name__)


def generate_trade_hash(*args) -> str:
    """Generate a deterministic unique trade ID using UUID v5"""
    TRADE_NAMESPACE = uuid.UUID("6ba7b810-9dad-11d1-80b4-00c04fd430c8")
    combined_string = "|".join(str(arg) for arg in args)
    return str(uuid.uuid5(TRADE_NAMESPACE, combined_string))


def normalize_instrument(instrument: str) -> str:
    """Normalize instrument name by removing last two chars if last char is a digit"""
    if instrument and instrument[-1].isdigit():
        return instrument[:-2]
    return instrument


def fetch_tick_details() -> List[dict]:
    """Fetch tick details from the database"""
    try:
        with db.get_cursor() as cursor:
            cursor.execute('SELECT * FROM "TickDetails"')
            tick_details = cursor.fetchall()
            logger.info(
                f"Successfully fetched {len(tick_details)} tick details from database"
            )
            return tick_details
    except Exception as e:
        logger.error(f"Failed to fetch tick details: {e}")
        logger.exception(e)
        return []


def calculate_pnl(
    side: str,
    quantity: int,
    entry_price: float,
    exit_price: float,
    ticker: str,
    tick_details: List[dict],
) -> float:
    """Calculate PnL for a trade"""
    try:
        normalized_order_ticker = normalize_instrument(ticker)
        logger.info(
            f"Calculating PnL - Original ticker: {ticker}, Normalized: {normalized_order_ticker}"
        )

        contract_spec = next(
            (
                detail
                for detail in tick_details
                if normalized_order_ticker == detail["ticker"]
            ),
            {"tickSize": 1 / 64, "tickValue": 15.625},  # Default values if not found
        )

        if contract_spec == {"tickSize": 1 / 64, "tickValue": 15.625}:
            logger.warning(
                f"No tick details found for {normalized_order_ticker} in database. Using default values. "
                f"Available tickers: {[detail['ticker'] for detail in tick_details]}"
            )

        price_difference = exit_price - entry_price
        raw_ticks = price_difference / contract_spec["tickSize"]
        ticks = round(raw_ticks)
        raw_pnl = ticks * contract_spec["tickValue"] * quantity
        final_pnl = raw_pnl if side == "Long" else -raw_pnl

        logger.info(
            f"PnL calculation for {ticker}: "
            f"price_diff={price_difference}, raw_ticks={raw_ticks}, ticks={ticks}, "
            f"quantity={quantity}, side={side}, final_pnl={round(final_pnl, 2)}"
        )
        return round(final_pnl, 2)
    except Exception as e:
        logger.error(f"Error calculating PnL: {e}")
        return 0.0


async def store_trades(trades: List[Trade], session_id: str = None) -> None:
    """Store trades in PostgreSQL database using transaction"""
    try:
        with db.get_cursor() as cursor:
            # Prepare all trade data as a list of tuples
            trade_data = [
                (
                    trade.id,
                    trade.userId,
                    trade.accountNumber,
                    trade.instrument,
                    trade.quantity,
                    trade.entryPrice,
                    trade.closePrice,
                    trade.entryDate,
                    trade.closeDate,
                    trade.side,
                    trade.commission,
                    trade.timeInPosition,
                    trade.pnl,
                    trade.entryId,
                    trade.closeId,
                    trade.comment,
                    trade.createdAt,
                )
                for trade in trades
            ]

            # Execute batch insert
            cursor.executemany(
                """
                INSERT INTO "Trade" (
                    "id", "userId", "accountNumber", "instrument", 
                    "quantity", "entryPrice", "closePrice", "entryDate", 
                    "closeDate", "side", "commission", "timeInPosition",
                    "pnl", "entryId", "closeId", "comment", "createdAt"
                ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                ON CONFLICT ("id") DO NOTHING
                """,
                trade_data,
            )

            # Commit the transaction
            cursor.connection.commit()
            logger.info(f"Successfully processed {len(trades)} trades")

            # Send final storage stats if we have a session
            if session_id:
                from app.services.websocket_service import ws_manager

                await ws_manager.broadcast_to_session(
                    session_id,
                    {
                        "type": "processing_complete",
                        "trades_count": len(trades),
                        "open_positions_count": 0,  # We don't have this info in store_trades
                        "message": (
                            "No trades found for the selected period"
                            if not trades
                            else f"Successfully processed {len(trades)} trades"
                        ),
                    },
                )

    except Exception as e:
        logger.error(f"Failed to store trades: {e}")
        logger.exception(e)
        if session_id:
            from app.services.websocket_service import ws_manager

            await ws_manager.broadcast_log(
                session_id,
                f"Error storing trades: {str(e)}",
                "error",
            )
        raise


def process_orders(
    orders_data: dict, user_id: str, tick_details: List[dict] = None
) -> Tuple[List[Trade], List[OpenPosition]]:
    """Process orders into trades and open positions"""
    try:
        if tick_details is None:
            tick_details = fetch_tick_details()

        positions_by_account: Dict[str, Dict[str, dict]] = {}
        processed_trades: List[Trade] = []
        processed_order_ids: set = set()
        open_positions: List[OpenPosition] = []

        if not isinstance(orders_data, dict):
            logger.error("Invalid orders data format: not a dictionary")
            return [], []

        account_ids = [
            key for key in orders_data.keys() if key not in ["status", "timestamp"]
        ]

        for account_id in account_ids:
            try:
                account_orders = orders_data[account_id]
                if not isinstance(account_orders, list):
                    logger.error(f"Invalid orders format for account {account_id}")
                    continue

                total_commission = sum(order["commission"] for order in account_orders)
                logger.info(
                    f"Total commission for account {account_id}: ${total_commission:.2f}"
                )

                sorted_orders = sorted(
                    [
                        order
                        for order in account_orders
                        if order["order_id"] not in processed_order_ids
                    ],
                    key=lambda x: x["timestamp"],
                )

                for order in sorted_orders:
                    if order["order_id"] in processed_order_ids:
                        continue

                    processed_order_ids.add(order["order_id"])
                    process_single_order(
                        order,
                        account_id,
                        user_id,
                        positions_by_account,
                        processed_trades,
                        tick_details,
                    )

            except Exception as e:
                logger.error(f"Error processing account {account_id}: {e}")
                continue

        # Get open positions
        open_positions = get_open_positions(positions_by_account)

        return processed_trades, open_positions

    except Exception as e:
        logger.error(f"Error processing orders: {e}")
        logger.exception(e)
        return [], []


def process_single_order(
    order: dict,
    account_id: str,
    user_id: str,
    positions_by_account: dict,
    processed_trades: List[Trade],
    tick_details: List[dict],
) -> None:
    """Process a single order and update trades and positions"""
    try:
        order_side = "Long" if order["side"] == "B" else "Short"
        instrument = normalize_instrument(order["symbol"])

        if account_id not in positions_by_account:
            positions_by_account[account_id] = {}
        if instrument not in positions_by_account[account_id]:
            positions_by_account[account_id][instrument] = {"queue": [], "side": None}

        position = positions_by_account[account_id][instrument]
        timestamp = datetime.fromtimestamp(
            order["timestamp"], tz=timezone.utc
        ).isoformat()

        lot_order = QueuedOrder(
            quantity=order["filled_quantity"],
            price=float(order["price"]),
            commission=float(order["commission"]),
            timestamp=timestamp,
            order_id=order["order_id"],
            side=order_side,
            remaining=order["filled_quantity"],
        )

        if not position["side"] or position["side"] == order_side:
            position["queue"].append(lot_order)
            position["side"] = order_side
            return

        process_closing_order(
            lot_order,
            position,
            account_id,
            instrument,
            user_id,
            processed_trades,
            tick_details,
        )

    except Exception as e:
        logger.error(f"Error processing single order: {e}")
        logger.exception(e)


def process_closing_order(
    lot_order: QueuedOrder,
    position: dict,
    account_id: str,
    instrument: str,
    user_id: str,
    processed_trades: List[Trade],
    tick_details: List[dict],
) -> None:
    """Process a closing order against existing position"""
    try:
        remaining_close_qty = lot_order.quantity

        while position["queue"] and remaining_close_qty > 0:
            opening_order = position["queue"][0]
            match_qty = min(opening_order.remaining, remaining_close_qty)

            entry_commission = (
                match_qty / opening_order.quantity
            ) * opening_order.commission
            exit_commission = (match_qty / lot_order.quantity) * lot_order.commission

            trade = create_trade(
                user_id,
                account_id,
                instrument,
                opening_order,
                lot_order,
                match_qty,
                entry_commission,
                exit_commission,
                position["side"],
                tick_details,
            )

            processed_trades.append(trade)

            opening_order.remaining -= match_qty
            remaining_close_qty -= match_qty

            if opening_order.remaining == 0:
                position["queue"].pop(0)

        if remaining_close_qty > 0:
            lot_order.remaining = remaining_close_qty
            position["queue"] = [lot_order]
            position["side"] = lot_order.side
        elif not position["queue"]:
            position["side"] = None

    except Exception as e:
        logger.error(f"Error processing closing order: {e}")
        logger.exception(e)


def create_trade(
    user_id: str,
    account_id: str,
    instrument: str,
    opening_order: QueuedOrder,
    closing_order: QueuedOrder,
    match_qty: int,
    entry_commission: float,
    exit_commission: float,
    side: str,
    tick_details: List[dict],
) -> Trade:
    """Create a trade object from matched orders"""
    try:
        return Trade(
            id=generate_trade_hash(
                user_id,
                account_id,
                instrument,
                opening_order.order_id,
                closing_order.order_id,
                match_qty,
            ),
            userId=user_id,
            accountNumber=account_id,
            instrument=instrument,
            quantity=match_qty,
            entryPrice=str(opening_order.price),
            closePrice=str(closing_order.price),
            entryDate=opening_order.timestamp,
            closeDate=closing_order.timestamp,
            side=side,
            commission=entry_commission + exit_commission,
            timeInPosition=float(
                (
                    datetime.fromisoformat(closing_order.timestamp)
                    - datetime.fromisoformat(opening_order.timestamp)
                ).total_seconds()
            ),
            pnl=calculate_pnl(
                side,
                match_qty,
                opening_order.price,
                closing_order.price,
                instrument,
                tick_details,
            ),
            entryId=opening_order.order_id,
            closeId=closing_order.order_id,
            comment=None,
            createdAt=datetime.now(timezone.utc),
        )
    except Exception as e:
        logger.error(f"Error creating trade: {e}")
        logger.exception(e)
        raise


def get_open_positions(positions_by_account: dict) -> List[OpenPosition]:
    """Get list of open positions from position data"""
    try:
        open_positions = []
        for account_id, instruments in positions_by_account.items():
            for instrument, position in instruments.items():
                if position["queue"]:
                    total_quantity = sum(order.remaining for order in position["queue"])
                    if total_quantity > 0:
                        weighted_price = (
                            sum(
                                order.price * order.remaining
                                for order in position["queue"]
                            )
                            / total_quantity
                        )
                        open_positions.append(
                            OpenPosition(
                                accountNumber=account_id,
                                instrument=instrument,
                                side=position["side"],
                                quantity=total_quantity,
                                entryPrice=weighted_price,
                                entryDate=position["queue"][0].timestamp,
                                commission=sum(
                                    order.commission for order in position["queue"]
                                ),
                                orderId=",".join(
                                    order.order_id for order in position["queue"]
                                ),
                            )
                        )
        return open_positions
    except Exception as e:
        logger.error(f"Error getting open positions: {e}")
        logger.exception(e)
        return []
