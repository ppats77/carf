FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY *.py ./
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
USER 65534:65534
CMD ["uvicorn", "receiver:app", "--host", "0.0.0.0", "--port", "8787", "--no-access-log", "--limit-concurrency", "32"]
