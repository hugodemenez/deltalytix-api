from fastapi import APIRouter, WebSocket, WebSocketDisconnect, HTTPException, Depends
from fastapi.security import HTTPBearer
import logging
import json
import uuid
from typing import List
import os
import asyncio

from app.core.config import settings
from app.core.security import create_session_token, verify_token
from app.models.trade import (
    Credentials,
    AccountListResponse,
    OrderRequest,
    OrderListResponse,
    ServerResponse,
)
from app.services.websocket_service import ws_manager
from app.services.trade_service import process_orders, store_trades
from app.services.account_service import execute_account_fetcher
from app.services.order_service import execute_order_fetcher
from app.api.routes import accounts, orders, servers, websocket, manual_orders

logger = logging.getLogger(__name__)
security = HTTPBearer()
router = APIRouter()

# Include all route modules
router.include_router(accounts.router, prefix="/accounts", tags=["accounts"])

router.include_router(orders.router, prefix="/orders", tags=["orders"])

router.include_router(servers.router, prefix="/servers", tags=["servers"])

router.include_router(websocket.router, prefix="/ws", tags=["websocket"])

router.include_router(
    manual_orders.router, prefix="/manual-orders", tags=["manual-orders"]
)
