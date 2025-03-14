import jwt
from datetime import datetime, timedelta
from typing import Optional
from fastapi import HTTPException
from app.core.config import settings


def create_session_token(user_id: str, session_id: str) -> str:
    """Create a JWT session token"""
    try:
        payload = {
            "user_id": user_id,
            "session_id": session_id,
            "exp": datetime.utcnow()
            + timedelta(minutes=settings.ACCESS_TOKEN_EXPIRE_MINUTES),
        }
        return jwt.encode(payload, settings.SECRET_KEY, algorithm="HS256")
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error creating token: {str(e)}")


def decode_session_token(token: str) -> dict:
    """Decode and validate a JWT session token"""
    try:
        return jwt.decode(token, settings.SECRET_KEY, algorithms=["HS256"])
    except jwt.ExpiredSignatureError:
        raise HTTPException(status_code=401, detail="Token has expired")
    except jwt.InvalidTokenError as e:
        raise HTTPException(status_code=401, detail=f"Invalid token: {str(e)}")
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error decoding token: {str(e)}")


def verify_token(token: str, session_id: Optional[str] = None) -> dict:
    """Verify token and optionally check session_id match"""
    payload = decode_session_token(token)
    if session_id and payload.get("session_id") != session_id:
        raise HTTPException(status_code=401, detail="Token session mismatch")
    return payload
