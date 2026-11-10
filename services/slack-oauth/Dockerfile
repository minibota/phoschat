FROM python:3.12-slim-bookworm

ENV PORT=8080 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
WORKDIR /app
RUN python -m pip install --no-cache-dir --disable-pip-version-check gunicorn==26.2.0
COPY relay.py /app/relay.py
USER 65532:65532
EXPOSE 8080
CMD ["gunicorn", "--workers", "1", "--threads", "4", "--timeout", "20", "relay:create_application()"]
