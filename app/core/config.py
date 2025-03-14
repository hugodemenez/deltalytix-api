from pydantic_settings import BaseSettings
from typing import Optional
import os


class Settings(BaseSettings):
    # API Settings
    API_V1_STR: str = "/api/v1"
    PROJECT_NAME: str = "DELTALYTIX API"

    # Database settings
    POSTGRES_DB: str = os.getenv("POSTGRES_DB", "")
    POSTGRES_USER: str = os.getenv("POSTGRES_USER", "")
    POSTGRES_PASSWORD: str = os.getenv("POSTGRES_PASSWORD", "")
    POSTGRES_HOST: str = os.getenv("POSTGRES_HOST", "")
    POSTGRES_PORT: str = os.getenv("POSTGRES_PORT", "6543")

    # Trading settings
    USERNAME_TEST: str = os.getenv("USERNAME_TEST", "")
    PASSWORD_TEST: str = os.getenv("PASSWORD_TEST", "")
    RITHMIC_ENV: str = os.getenv("RITHMIC_ENV", "TEST")

    # Security
    SECRET_KEY: str = os.getenv("JWT_SECRET_KEY", "your-secret-key")
    ACCESS_TOKEN_EXPIRE_MINUTES: int = 60 * 24  # 24 hours

    # CORS Settings
    BACKEND_CORS_ORIGINS: list = ["*"]  # In production, replace with specific origins

    class Config:
        case_sensitive = True
        env_file = ".env"


settings = Settings()


def verify_environment():
    """Verify all required environment variables are set"""
    required_vars = [
        "POSTGRES_DB",
        "POSTGRES_USER",
        "POSTGRES_PASSWORD",
        "POSTGRES_HOST",
        "POSTGRES_PORT",
        "USERNAME_TEST",
        "PASSWORD_TEST",
        "RITHMIC_ENV",
    ]

    missing_vars = [var for var in required_vars if not getattr(settings, var)]
    if missing_vars:
        raise ValueError(
            f"Missing required environment variables: {', '.join(missing_vars)}"
        )
