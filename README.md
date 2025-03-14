# Deltalytix API

Deltalytix is a web-based trading journal specifically designed for futures traders. This repository contains the backend API implementation that powers the Deltalytix platform.

## Overview

The Deltalytix API is built using FastAPI and provides both REST and WebSocket endpoints. It includes C++ bindings to connect with the Rithmic RAPI+ API, enabling direct integration with Rithmic's trading infrastructure. The architecture is designed to be extensible, with plans to support additional trading providers in the future.

## Features

- FastAPI-based REST and WebSocket API
- Direct integration with Rithmic RAPI+ through C++ bindings
- Extensible architecture for future provider integrations
- Real-time trading data streaming capabilities
- Docker-based deployment with automatic SSL certificate management
- Redis for caching and real-time data handling
- Nginx reverse proxy with automatic SSL renewal

## Prerequisites

- Python 3.8+ (for local development)
- C++ compiler with C++17 support (for local development)
- Rithmic RAPI+ credentials and API access
- Docker and Docker Compose (for containerized deployment)
- Supabase account and project (for database and authentication)
- Domain name (for production deployment)

## Project Structure

```
deltalytix-api/
├── api/              # FastAPI application
├── bindings/         # C++ bindings for RAPI+
├── tests/           # Test suite
├── docs/            # Documentation
├── nginx/           # Nginx configuration and SSL setup
├── Dockerfile       # Main application container
├── Dockerfile.builder # C++ build container
├── .env.example     # Environment variables template
└── docker-compose.yml # Container orchestration
```

## Setup

### Environment Configuration

1. Copy the environment template:
```bash
cp .env.example .env
```

2. Edit the `.env` file with your credentials:
   - Rithmic API credentials from your Rithmic account
   - Supabase project URL and keys from your Supabase dashboard
   - Database credentials from your Supabase project
   - GitHub token if you need to access private packages
   - Domain name and email for SSL certificates (for production)

⚠️ **Important**: Never commit your `.env` file to version control. It's already included in `.gitignore`.

### Local Development Setup

1. Clone the repository:
```bash
git clone https://github.com/yourusername/deltalytix-api.git
cd deltalytix-api
```

2. Create and activate a virtual environment:
```bash
python -m venv venv
source venv/bin/activate  # On Windows: venv\Scripts\activate
```

3. Install dependencies:
```bash
pip install -r requirements.txt
```

4. Build C++ bindings:
```bash
cd bindings
mkdir build && cd build
cmake ..
make
```

### Docker-based Setup

1. Clone the repository:
```bash
git clone https://github.com/yourusername/deltalytix-api.git
cd deltalytix-api
```

2. Build and start the services:
```bash
docker-compose up -d
```

The services will be available at:
- API: `http://localhost:8000`
- Nginx (with SSL): `https://your-domain.com`

## Running the API

### Local Development
Start the FastAPI server:
```bash
uvicorn api.main:app --reload
```

The API will be available at `http://localhost:8000`

### Docker Deployment
The API is automatically started and managed by Docker Compose. You can:
- View logs: `docker-compose logs -f order-service`
- Restart services: `docker-compose restart order-service`
- Stop all services: `docker-compose down`

## API Documentation

Once the server is running, you can access:
- Interactive API docs (Swagger UI): `http://localhost:8000/docs`
- Alternative API docs (ReDoc): `http://localhost:8000/redoc`

## Development

### Running Tests
```bash
pytest
```

### Code Style
This project follows PEP 8 guidelines. Format your code using:
```bash
black .
isort .
```

## Docker Services

The application is composed of several Docker services:

- `order-service`: Main FastAPI application
- `cpp-builder`: Builds C++ bindings for RAPI+
- `nginx`: Reverse proxy with SSL termination
- `redis`: Caching and real-time data handling
- `certbot`: SSL certificate management
- `cleanup-service`: Automatic cleanup of old order files

## Contributing

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the GNU Affero General Public License v3 (AGPL-3.0) - see the [LICENSE](LICENSE) file for details. This license requires that if you run a modified version of this software on a server, you must make the source code available to the users of that server.

## Support

For support, please open an issue in the GitHub repository or contact the maintainers.
