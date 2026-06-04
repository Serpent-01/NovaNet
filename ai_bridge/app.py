import json
import os
from typing import AsyncIterator, List, Optional

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel, Field


load_dotenv()


class ChatMessage(BaseModel):
    role: str = Field(..., min_length=1)
    content: str = Field(..., min_length=1)


class GenerateRequest(BaseModel):
    request_id: Optional[str] = None
    stream_id: Optional[str] = None

    model: Optional[str] = None
    messages: List[ChatMessage]

    temperature: float = 0.7
    max_tokens: int = 1024


class BridgeConfig:
    host: str = os.getenv("AI_BRIDGE_HOST", "127.0.0.1")
    port: int = int(os.getenv("AI_BRIDGE_PORT", "18080"))

    upstream_url: str = os.getenv(
        "AI_BRIDGE_UPSTREAM_URL",
        "https://api.openai.com/v1/chat/completions",
    )
    api_key: str = os.getenv("AI_BRIDGE_API_KEY", "")
    model: str = os.getenv("AI_BRIDGE_MODEL", "gpt-4o-mini")

    connect_timeout_seconds: float = float(
        os.getenv("AI_BRIDGE_CONNECT_TIMEOUT_SECONDS", "3")
    )
    read_timeout_seconds: float = float(
        os.getenv("AI_BRIDGE_READ_TIMEOUT_SECONDS", "60")
    )

    max_messages: int = int(os.getenv("AI_BRIDGE_MAX_MESSAGES", "64"))
    max_content_chars: int = int(os.getenv("AI_BRIDGE_MAX_CONTENT_CHARS", "20000"))


config = BridgeConfig()
app = FastAPI(title="NovaNet Minimal AI Bridge")


def ndjson_line(obj: dict) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n"


def make_error_line(code: str, message: str) -> str:
    return ndjson_line(
        {
            "type": "error",
            "code": code,
            "message": message,
        }
    )


def validate_generate_request(req: GenerateRequest) -> Optional[str]:
    if not req.messages:
        return "messages is empty"

    if len(req.messages) > config.max_messages:
        return f"too many messages: {len(req.messages)} > {config.max_messages}"

    total_chars = sum(len(m.content) for m in req.messages)
    if total_chars > config.max_content_chars:
        return f"message content too large: {total_chars} > {config.max_content_chars}"

    return None


def build_upstream_payload(req: GenerateRequest) -> dict:
    return {
        "model": req.model or config.model,
        "messages": [
            {
                "role": m.role,
                "content": m.content,
            }
            for m in req.messages
        ],
        "temperature": req.temperature,
        "max_tokens": req.max_tokens,
        "stream": True,
    }


def extract_delta_from_openai_compatible_event(event: dict) -> tuple[str, Optional[str]]:
    """
    Expected upstream streaming event shape:

    {
      "choices": [
        {
          "delta": {"content": "..."},
          "finish_reason": null
        }
      ]
    }

    Some providers may slightly differ. This function intentionally keeps
    the first version simple.
    """
    choices = event.get("choices")
    if not choices:
        return "", None

    first = choices[0] or {}
    delta_obj = first.get("delta") or {}

    delta = delta_obj.get("content") or ""
    finish_reason = first.get("finish_reason")

    return delta, finish_reason


async def stream_from_real_ai(
    request: Request,
    req: GenerateRequest,
) -> AsyncIterator[str]:
    validation_error = validate_generate_request(req)
    if validation_error:
        yield make_error_line("invalid_request", validation_error)
        return

    if not config.api_key:
        yield make_error_line(
            "missing_api_key",
            "AI_BRIDGE_API_KEY is empty. Please set it in .env.",
        )
        return

    payload = build_upstream_payload(req)

    headers = {
        "Authorization": f"Bearer {config.api_key}",
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
    }

    timeout = httpx.Timeout(
        connect=config.connect_timeout_seconds,
        read=config.read_timeout_seconds,
        write=10.0,
        pool=config.connect_timeout_seconds,
    )

    index = 0
    saw_end = False

    try:
        async with httpx.AsyncClient(timeout=timeout) as client:
            async with client.stream(
                "POST",
                config.upstream_url,
                headers=headers,
                json=payload,
            ) as response:
                if response.status_code < 200 or response.status_code >= 300:
                    body = await response.aread()
                    text = body.decode("utf-8", errors="replace")
                    yield make_error_line(
                        "upstream_http_error",
                        f"upstream status={response.status_code}, body={text[:1000]}",
                    )
                    return

                async for raw_line in response.aiter_lines():
                    if await request.is_disconnected():
                        # NovaNet GatewayAiProvider closed the HTTP stream.
                        # Leaving this generator closes the upstream HTTP stream too.
                        return

                    line = raw_line.strip()
                    if not line:
                        continue

                    # Ignore SSE comments.
                    if line.startswith(":"):
                        continue

                    # OpenAI-compatible streaming usually uses:
                    # data: {...}
                    if not line.startswith("data:"):
                        continue

                    data = line[len("data:") :].strip()

                    if data == "[DONE]":
                        saw_end = True
                        yield ndjson_line(
                            {
                                "type": "end",
                                "finish_reason": "stop",
                            }
                        )
                        return

                    try:
                        event = json.loads(data)
                    except json.JSONDecodeError:
                        yield make_error_line(
                            "bad_upstream_event",
                            f"failed to decode upstream event: {data[:500]}",
                        )
                        return

                    delta, finish_reason = extract_delta_from_openai_compatible_event(
                        event
                    )

                    if delta:
                        yield ndjson_line(
                            {
                                "type": "chunk",
                                "index": index,
                                "delta": delta,
                                "finish_reason": "",
                            }
                        )
                        index += 1

                    if finish_reason:
                        saw_end = True
                        yield ndjson_line(
                            {
                                "type": "end",
                                "finish_reason": finish_reason,
                            }
                        )
                        return

        if not saw_end:
            yield ndjson_line(
                {
                    "type": "end",
                    "finish_reason": "stop",
                }
            )

    except httpx.TimeoutException as ex:
        yield make_error_line("upstream_timeout", str(ex))
    except httpx.ConnectError as ex:
        yield make_error_line("upstream_connect_error", str(ex))
    except httpx.HTTPError as ex:
        yield make_error_line("upstream_http_error", str(ex))
    except Exception as ex:
        yield make_error_line("bridge_internal_error", str(ex))


@app.get("/health")
async def health() -> dict:
    return {
        "ok": True,
        "service": "novanet-ai-bridge",
    }


@app.post("/chat/stream")
async def chat_stream(request: Request, req: GenerateRequest):
    return StreamingResponse(
        stream_from_real_ai(request, req),
        media_type="application/x-ndjson; charset=utf-8",
    )


@app.exception_handler(Exception)
async def unhandled_exception_handler(_: Request, exc: Exception):
    return JSONResponse(
        status_code=500,
        content={
            "ok": False,
            "error": str(exc),
        },
    )


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "app:app",
        host=config.host,
        port=config.port,
        reload=False,
        log_level="info",
    )