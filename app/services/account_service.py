import logging
import asyncio
from typing import List
from app.models.trade import Credentials, AccountData
import os
import json

logger = logging.getLogger(__name__)


async def execute_account_fetcher(credentials: Credentials) -> List[AccountData]:
    """Execute the GetAccountList executable"""
    try:
        # Define possible executable paths in order of preference
        executable_paths = [
            os.path.join(os.environ.get("PATH", "").split(":")[0], "GetAccountList"),
            "/app/bin/GetAccountList",
            "/app/GetAccountList",
            "./GetAccountList",
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
                f"Failed to find GetAccountList executable. Searched in: {', '.join(executable_paths)}"
            )

        # Get the directory containing the executable
        exec_dir = os.path.dirname(executable_path)

        command = [
            executable_path,
            credentials.username,
            credentials.password,
            credentials.server_type,
            credentials.location,
        ]

        # Log command (with password masked)
        safe_command = command.copy()
        safe_command[2] = "********"
        logger.info(f"Executing command: {' '.join(safe_command)}")

        # Set up environment
        env = os.environ.copy()
        lib_path = os.path.join(exec_dir, "lib")
        if "LD_LIBRARY_PATH" in env:
            env["LD_LIBRARY_PATH"] = f"{lib_path}:{env['LD_LIBRARY_PATH']}"
        else:
            env["LD_LIBRARY_PATH"] = lib_path

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

            accounts = []
            error_message = None

            stdout, stderr = await process.communicate()

            # Log both stdout and stderr for debugging
            if stdout:
                logger.debug(f"stdout: {stdout.decode()}")
            if stderr:
                logger.warning(f"stderr: {stderr.decode()}")

            # Process output
            for line in stdout.decode().split("\n"):
                if not line:
                    continue
                try:
                    data = json.loads(line.strip())
                    if data.get("type") == "accounts":
                        accounts.extend(
                            [
                                AccountData(**account)
                                for account in data.get("accounts", [])
                            ]
                        )
                    elif data.get("type") == "log" and data.get("level") == "error":
                        error_message = data.get("message")
                except json.JSONDecodeError as e:
                    logger.warning(f"Failed to parse JSON line: {line}, Error: {e}")
                    continue

            if process.returncode != 0 or error_message:
                error_details = f"Exit code: {process.returncode}"
                if error_message:
                    error_details += f", Message: {error_message}"
                if stderr:
                    error_details += f", stderr: {stderr.decode()}"
                raise RuntimeError(f"Failed to retrieve accounts. {error_details}")

            logger.info(f"Successfully retrieved {len(accounts)} accounts")
            return accounts

        finally:
            # Change back to original directory
            os.chdir(original_dir)

    except Exception as e:
        logger.error(f"Error executing account fetcher: {e}")
        logger.exception(e)
        raise
