from fastapi import APIRouter, HTTPException, BackgroundTasks
import logging
from app.models.trade import (
    OrderRequest,
    OrderListResponse,
    ProcessOrderResponse,
    ProcessStatusResponse,
)
from app.services.trade_service import process_orders, store_trades
from app.services.order_service import (
    execute_order_fetcher,
    process_orders_async,
    processing_tasks,
)

logger = logging.getLogger(__name__)
router = APIRouter()


@router.post("", response_model=OrderListResponse)
async def get_orders(request: OrderRequest):
    """Get list of orders for specified accounts"""
    try:
        # Execute order fetcher
        orders_data = await execute_order_fetcher(request)

        # Process orders
        trades, open_positions = process_orders(orders_data, request.username)

        # Store trades
        if trades:
            store_trades(trades)

        return OrderListResponse(
            success=True,
            message=f"Successfully processed {len(trades)} trades",
            orders=orders_data,
            accounts_processed=len(request.account_ids or []),
            total_accounts_available=len(request.account_ids or []),
        )

    except Exception as e:
        logger.error(f"Error retrieving orders: {e}")
        logger.exception(e)
        raise HTTPException(status_code=500, detail=str(e))


@router.post("/process", response_model=ProcessOrderResponse)
async def process_orders(request: OrderRequest, background_tasks: BackgroundTasks):
    """
    Start asynchronous order processing without waiting for completion.
    The actual processing will happen in the background.
    """
    try:
        logger.info(f"Received order processing request for user {request.userId}")
        logger.debug(
            f"Request details: server={request.server_name}, start_date={request.start_date}, accounts={request.account_ids}"
        )

        # Validate request
        if not request.userId:
            raise HTTPException(status_code=400, detail="userId is required")
        if not request.start_date:
            raise HTTPException(status_code=400, detail="start_date is required")
        if not request.server_name:
            raise HTTPException(status_code=400, detail="server_name is required")

        # Generate a unique process ID for this request
        process_id = await process_orders_async(request, background_tasks)
        logger.info(f"Started order processing with process ID: {process_id}")

        return ProcessOrderResponse(
            success=True,
            message="Order processing started successfully",
            process_id=process_id,
        )
    except Exception as e:
        logger.error(f"Failed to start order processing: {e}")
        if isinstance(e, HTTPException):
            raise
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/process/{process_id}", response_model=ProcessStatusResponse)
async def get_process_status(process_id: str):
    """Get the status of an order processing task"""
    if process_id not in processing_tasks:
        raise HTTPException(status_code=404, detail="Process not found")

    task_info = processing_tasks[process_id]
    return ProcessStatusResponse(
        process_id=process_id,
        status=task_info["status"],
        started_at=task_info["started_at"],
        completed_at=task_info["completed_at"],
        error=task_info["error"],
        result=task_info.get("result"),
    )
