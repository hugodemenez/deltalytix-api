import logging
import asyncio
import json
import os
import uuid
from typing import Dict, List
from fastapi import BackgroundTasks
from app.models.trade import OrderRequest
from app.services.websocket_service import ws_manager
from app.services.trade_service import process_orders, store_trades
from app.services.rithmic_orders_retrieval import retrieve_rithmic_orders
from datetime import datetime
import shutil

logger = logging.getLogger(__name__)

# Store background tasks state
processing_tasks: Dict[str, dict] = {}


async def execute_order_fetcher(request: OrderRequest, session_id: str = None) -> Dict:
    """Execute the Rithmic orders retrieval service"""
    try:
        # Use Rithmic orders retrieval service
        orders_data = await retrieve_rithmic_orders(request, session_id)

        if not orders_data:
            raise RuntimeError("No orders data returned")

        num_accounts = len(
            [k for k in orders_data.keys() if k not in ["status", "timestamp"]]
        )
        logger.info(f"Successfully retrieved orders for {num_accounts} accounts")
        return orders_data

    except Exception as e:
        logger.error(f"Error executing order fetcher: {e}")
        logger.exception(e)
        raise


async def process_websocket_data(
    session_id: str, selected_accounts: List[str], data: dict
):
    """Process WebSocket data and handle order processing"""
    try:
        state = ws_manager.get_session_state(session_id)
        if not state or not state.credentials:
            raise ValueError("No session state or credentials found")

        credentials = state.credentials
        start_date = data.get("start_date")
        if start_date:
            credentials["start_date"] = start_date

        # Create order request with userId from the data
        request = OrderRequest(
            username=credentials["username"],
            password=credentials["password"],
            server_type=credentials["server_type"],
            server_name=credentials["location"],  # Use location as server_name
            start_date=credentials["start_date"],
            account_ids=selected_accounts,
            userId=data["userId"],  # Add userId from the data
        )

        # Log start of processing
        await ws_manager.broadcast_status(
            session_id,
            f"Starting order processing for {len(selected_accounts)} accounts",
        )

        # Use Rithmic orders retrieval service instead of executable
        orders_data = await retrieve_rithmic_orders(request, session_id)

        # Process orders and broadcast results
        await ws_manager.broadcast_to_session(
            session_id,
            {
                "type": "orders",
                "data": orders_data,
                "message": f"Retrieved orders for {len(selected_accounts)} accounts",
            },
        )

        # Process each order and broadcast updates
        for account_id, account_orders in orders_data.items():
            if isinstance(account_orders, list):  # Skip metadata fields
                for order in account_orders:
                    await ws_manager.broadcast_to_session(
                        session_id, {"type": "order_update", "order": order}
                    )

        # Process orders into trades
        await ws_manager.broadcast_status(
            session_id,
            "Processing orders into trades...",
        )
        trades, open_positions = process_orders(
            orders_data,
            data["userId"],
            None,  # Let process_orders fetch tick details
        )

        # Send trade processing results
        await ws_manager.broadcast_to_session(
            session_id,
            {
                "type": "trades_processed",
                "trades_count": len(trades),
                "open_positions_count": len(open_positions),
                "message": f"Processed {len(trades)} trades and found {len(open_positions)} open positions",
            },
        )

        # Store trades in database
        if trades:
            await ws_manager.broadcast_status(
                session_id,
                f"Storing {len(trades)} trades in database...",
            )
            await store_trades(trades, session_id)
            await ws_manager.broadcast_status(
                session_id,
                f"Successfully stored {len(trades)} trades in database",
                "success",
            )

        # Send completion status
        await ws_manager.broadcast_status(
            session_id,
            f"Successfully processed orders for {len(selected_accounts)} accounts",
            "success",
        )

    except Exception as e:
        logger.error(f"Error processing websocket data: {e}")
        logger.exception(e)
        await ws_manager.broadcast_log(
            session_id, f"Error processing orders: {str(e)}", "error"
        )


async def process_orders_async(
    request: OrderRequest, background_tasks: BackgroundTasks
) -> str:
    """Start asynchronous order processing and return a process ID"""
    process_id = str(uuid.uuid4())

    # Initialize task state
    processing_tasks[process_id] = {
        "status": "pending",
        "request": request,
        "started_at": None,
        "completed_at": None,
        "error": None,
    }

    # Add the processing task to background tasks
    background_tasks.add_task(process_orders_background, process_id, request)

    return process_id


async def process_orders_background(process_id: str, request: OrderRequest):
    """Background task for processing orders"""
    try:
        processing_tasks[process_id]["status"] = "running"
        processing_tasks[process_id]["started_at"] = datetime.now()

        # Use Rithmic orders retrieval service instead of executable
        orders_data = await retrieve_rithmic_orders(request)

        if not orders_data:
            logger.warning(f"No orders data returned for process {process_id}")
            processing_tasks[process_id]["status"] = "completed"
            processing_tasks[process_id]["completed_at"] = datetime.now()
            processing_tasks[process_id]["result"] = {
                "trades_count": 0,
                "open_positions_count": 0,
                "message": "No orders data returned",
            }
            return

        # Initialize variables
        trades = []
        open_positions = []

        # Process orders
        try:
            result = process_orders(
                orders_data,
                request.userId,
                None,  # Let process_orders fetch tick details
            )

            if result and isinstance(result, tuple) and len(result) == 2:
                trades, open_positions = result
            else:
                logger.warning(
                    f"Unexpected result from process_orders for {process_id}: {result}"
                )
                trades = []
                open_positions = []
        except Exception as e:
            logger.error(f"Error processing orders for {process_id}: {e}")
            processing_tasks[process_id]["status"] = "error"
            processing_tasks[process_id]["error"] = f"Error processing orders: {str(e)}"
            raise

        # Store trades in database if we have any valid trades
        if trades and isinstance(trades, list) and len(trades) > 0:
            try:
                await store_trades(trades, request.userId)
                logger.info(
                    f"Successfully stored {len(trades)} trades for process {process_id}"
                )
            except Exception as e:
                logger.error(f"Error storing trades for {process_id}: {e}")
                processing_tasks[process_id]["status"] = "error"
                processing_tasks[process_id][
                    "error"
                ] = f"Error storing trades: {str(e)}"
                raise
        else:
            logger.info(f"No trades to store for process {process_id}")

        # Ensure we have valid lists for the result
        trades = trades if isinstance(trades, list) else []
        open_positions = open_positions if isinstance(open_positions, list) else []

        processing_tasks[process_id]["status"] = "completed"
        processing_tasks[process_id]["completed_at"] = datetime.now()
        processing_tasks[process_id]["result"] = {
            "trades_count": len(trades),
            "open_positions_count": len(open_positions),
            "message": "Processing completed successfully",
        }

    except Exception as e:
        logger.error(f"Error in background processing for {process_id}: {e}")
        logger.exception(e)
        processing_tasks[process_id]["status"] = "error"
        processing_tasks[process_id]["error"] = str(e)
        raise
