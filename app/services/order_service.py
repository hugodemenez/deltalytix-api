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
from datetime import datetime
import shutil

logger = logging.getLogger(__name__)

# Store background tasks state
processing_tasks: Dict[str, dict] = {}


async def execute_order_fetcher(request: OrderRequest, session_id: str = None) -> Dict:
    """Execute the OrderFetcher executable"""
    try:
        # Define possible executable paths in order of preference
        executable_paths = [
            os.path.join(os.environ.get("PATH", "").split(":")[0], "OrderFetcher"),
            "/app/bin/OrderFetcher",
            "/app/OrderFetcher",
            "./OrderFetcher",
        ]

        # Log environment for debugging
        logger.info(f"Current working directory: {os.getcwd()}")
        logger.info(f"PATH environment: {os.environ.get('PATH', 'not set')}")
        logger.info(
            f"LD_LIBRARY_PATH environment: {os.environ.get('LD_LIBRARY_PATH', 'not set')}"
        )
        logger.info("Checking executable paths:")

        executable_path = None
        for path in executable_paths:
            logger.info(f"Checking path: {path}")
            if os.path.exists(path):
                try:
                    # Check if file is executable
                    if os.access(path, os.X_OK):
                        executable_path = path
                        logger.info(f"Found executable at: {path}")
                        break
                    else:
                        logger.warning(f"File exists but is not executable: {path}")
                except Exception as e:
                    logger.warning(f"Error checking path {path}: {e}")
            else:
                logger.info(f"Path does not exist: {path}")

        if not executable_path:
            raise FileNotFoundError(
                f"Failed to find OrderFetcher executable. Searched in: {', '.join(executable_paths)}"
            )

        # Get the directory containing the executable
        exec_dir = os.path.dirname(executable_path)

        # Validate date format
        try:
            datetime.strptime(request.start_date, "%Y%m%d")
        except ValueError:
            raise ValueError("Invalid date format. Please use YYYYMMDD format.")

        # Build command
        command = [
            executable_path,
            request.username,
            request.password,
            request.server_type,  # Use server_type (e.g., "SpeedUp")
            request.server_name,  # Use server_name as location (e.g., "Chicago Area")
            request.start_date,
        ]

        if request.account_ids:
            command.extend(request.account_ids)

        # Set up environment
        env = os.environ.copy()
        lib_path = os.path.join(exec_dir, "lib")
        if "LD_LIBRARY_PATH" in env:
            env["LD_LIBRARY_PATH"] = f"{lib_path}:{env['LD_LIBRARY_PATH']}"
        else:
            env["LD_LIBRARY_PATH"] = lib_path

        # Log command (with password masked)
        safe_command = command.copy()
        safe_command[2] = "********"
        logger.info(f"Executing command: {' '.join(safe_command)}")
        logger.info(
            f"Using server type: {request.server_type} with location: {request.server_name}"
        )
        logger.info(f"Using LD_LIBRARY_PATH: {env['LD_LIBRARY_PATH']}")

        # Change to executable directory to ensure it can find its dependencies
        original_dir = os.getcwd()
        os.chdir(exec_dir)

        try:
            process = await asyncio.create_subprocess_exec(
                *command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env=env,
            )

            orders_data = {}
            error_message = None
            orders_file = None

            # Create tasks to read stdout and stderr streams
            async def read_stream(stream, is_stderr=False):
                while True:
                    line = await stream.readline()
                    if not line:
                        break

                    decoded_line = line.decode().strip()
                    if not decoded_line:
                        continue

                    try:
                        data = json.loads(decoded_line)
                        # Forward all messages to the client if we have a session_id
                        if session_id:
                            await ws_manager.broadcast_to_session(session_id, data)

                        if data.get("type") == "complete" and "orders_file" in data:
                            # Get the orders file path
                            nonlocal orders_file
                            orders_file = os.path.join(exec_dir, data["orders_file"])
                            logger.info(f"Orders file written to: {orders_file}")

                            # Read orders from the file
                            if os.path.exists(orders_file):
                                with open(orders_file, "r") as f:
                                    orders_data.update(json.load(f))
                                logger.info(
                                    f"Successfully loaded orders from {orders_file}"
                                )
                        elif data.get("type") == "log" and data.get("level") == "error":
                            error_message = data.get("message")
                    except json.JSONDecodeError:
                        # For non-JSON output, send as log message
                        if session_id:
                            log_message = {
                                "type": "log",
                                "level": "error" if is_stderr else "info",
                                "message": decoded_line,
                                "timestamp": int(datetime.now().timestamp()),
                            }
                            await ws_manager.broadcast_to_session(
                                session_id, log_message
                            )

            # Wait for both streams to complete
            await asyncio.gather(
                read_stream(process.stdout, False), read_stream(process.stderr, True)
            )

            # Wait for process to complete
            await process.wait()

            if process.returncode != 0 or error_message:
                error_details = f"Exit code: {process.returncode}"
                if error_message:
                    error_details += f", Message: {error_message}"
                raise RuntimeError(f"Failed to retrieve orders. {error_details}")

            num_accounts = len(
                [k for k in orders_data.keys() if k not in ["status", "timestamp"]]
            )
            logger.info(f"Successfully retrieved orders for {num_accounts} accounts")
            return orders_data

        finally:
            # Change back to original directory
            os.chdir(original_dir)

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

        # Execute order fetcher with session_id for real-time logging
        orders_data = await execute_order_fetcher(request, session_id)

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

        # Execute order fetcher
        orders_data = await execute_order_fetcher(request)

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
