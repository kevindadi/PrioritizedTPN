"""Provider adapters and JSON-response helpers for the LLM front-end.

Only the Python standard library is used.  Network access happens exclusively
inside :meth:`AnthropicClient.complete` and :meth:`OpenAIClient.complete`; tests
use :class:`FakeClient` or pass an injectable callable.

API keys are read from the environment and are never included in exception
messages, logs, or ``repr`` output.
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple, Union

ANTHROPIC_API_URL = "https://api.anthropic.com/v1/messages"
ANTHROPIC_VERSION = "2023-06-01"
OPENAI_API_URL = "https://api.openai.com/v1/chat/completions"

PROVIDER_ENV = "PTPN_LLM_PROVIDER"
MODEL_ENV = "PTPN_LLM_MODEL"
BASE_URL_ENV = "PTPN_LLM_BASE_URL"
ANTHROPIC_KEY_ENV = "ANTHROPIC_API_KEY"
OPENAI_KEY_ENV = "OPENAI_API_KEY"

ANTHROPIC_PROVIDER = "anthropic"
OPENAI_PROVIDER = "openai"
SUPPORTED_PROVIDERS = (ANTHROPIC_PROVIDER, OPENAI_PROVIDER)


class LLMError(RuntimeError):
    """Base class for LLM front-end errors."""


class ConfigError(LLMError):
    """Raised when provider configuration is missing or invalid."""


class ClientError(LLMError):
    """Raised when a provider request or response is unusable."""


class ResponseParseError(LLMError):
    """Raised when a model response contains no usable JSON object."""


class _StrictJSONError(ValueError):
    """Raised for duplicate keys or non-standard constants in JSON."""


def _object_pairs_without_duplicates(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result = {}  # type: Dict[str, Any]
    for key, value in pairs:
        if key in result:
            raise _StrictJSONError("duplicate JSON object key: {!r}".format(key))
        result[key] = value
    return result


def _reject_nonstandard_constant(value: str) -> None:
    raise _StrictJSONError("non-standard JSON constant: {!r}".format(value))


def strict_json_loads(text: str) -> Any:
    """Parse JSON while rejecting duplicate keys and NaN/Infinity literals."""
    return json.loads(
        text,
        object_pairs_hook=_object_pairs_without_duplicates,
        parse_constant=_reject_nonstandard_constant,
    )


def extract_json_object(text: str) -> Dict[str, Any]:
    """Return the first JSON object embedded in *text*.

    Prose and Markdown fences are tolerated.  Brace matching is delegated to
    :class:`json.JSONDecoder`, so braces inside JSON strings are handled
    correctly.
    """
    if not isinstance(text, str):
        raise TypeError("model response must be a string")

    decoder = json.JSONDecoder(
        object_pairs_hook=_object_pairs_without_duplicates,
        parse_constant=_reject_nonstandard_constant,
    )
    index = text.find("{")
    while index != -1:
        try:
            value, _ = decoder.raw_decode(text, index)
        except _StrictJSONError as exc:
            raise ResponseParseError(str(exc))
        except json.JSONDecodeError:
            index = text.find("{", index + 1)
            continue
        if isinstance(value, dict):
            return value
        index = text.find("{", index + 1)

    raise ResponseParseError("no JSON object found in model response")


def _validate_base_url(base_url: str) -> str:
    parsed = urllib.parse.urlsplit(base_url)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise ConfigError(
            "invalid base URL {!r}: expected an http(s) URL with a host".format(base_url)
        )
    if parsed.username is not None or parsed.password is not None:
        raise ConfigError("base URL must not embed credentials")
    return base_url.rstrip("/")


def anthropic_endpoint(base_url: Optional[str] = None) -> str:
    """Resolve the Anthropic Messages endpoint from an optional base URL."""
    if base_url is None:
        return ANTHROPIC_API_URL
    base = _validate_base_url(base_url)
    if base.endswith("/v1/messages"):
        return base
    return base + "/v1/messages"


def openai_endpoint(base_url: Optional[str] = None) -> str:
    """Resolve the OpenAI-compatible chat-completions endpoint."""
    if base_url is None:
        return OPENAI_API_URL
    base = _validate_base_url(base_url)
    if base.endswith("/v1/chat/completions"):
        return base
    if base.endswith("/v1"):
        return base + "/chat/completions"
    return base + "/v1/chat/completions"


def _resolve_api_key(
    explicit: Optional[str],
    env: Optional[Mapping[str, str]],
    env_name: str,
) -> str:
    if explicit is not None:
        key = explicit
    else:
        source = os.environ if env is None else env
        key = source.get(env_name)
    if not key:
        raise ConfigError(
            "{} is not set; export the provider API key in the environment".format(env_name)
        )
    return key


class _BaseHTTPClient:
    provider = ""
    key_env = ""

    def __init__(
        self,
        model: str,
        api_key: Optional[str] = None,
        base_url: Optional[str] = None,
        timeout: float = 60.0,
        max_tokens: int = 8192,
        env: Optional[Mapping[str, str]] = None,
    ) -> None:
        if not isinstance(model, str) or not model.strip():
            raise ConfigError("model name must be a non-empty string")
        if timeout <= 0:
            raise ConfigError("timeout must be positive")
        if not isinstance(max_tokens, int) or isinstance(max_tokens, bool) or max_tokens <= 0:
            raise ConfigError("max_tokens must be a positive integer")
        self.model = model
        self.timeout = float(timeout)
        self.max_tokens = max_tokens
        self.base_url = base_url
        self._api_key = _resolve_api_key(api_key, env, self.key_env)

    def __repr__(self) -> str:
        return "{}(model={!r}, base_url={!r})".format(
            type(self).__name__, self.model, self.base_url
        )

    def build_request(self, prompt: str) -> Tuple[str, Dict[str, str], Dict[str, Any]]:
        raise NotImplementedError

    def parse_response(self, payload: Dict[str, Any]) -> str:
        raise NotImplementedError

    def complete(self, prompt: str) -> str:
        """Send *prompt* to the provider and return the model's text response."""
        if not isinstance(prompt, str):
            raise TypeError("prompt must be a string")
        url, headers, payload = self.build_request(prompt)
        body = json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(url, data=body, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
        except urllib.error.HTTPError as exc:
            detail = _decode_http_error(exc)
            raise ClientError(
                "{} request failed with HTTP {}: {}".format(self.provider, exc.code, detail)
            )
        except urllib.error.URLError as exc:
            raise ClientError(
                "{} request failed: {}".format(self.provider, exc.reason)
            )
        except OSError as exc:
            raise ClientError("{} request failed: {}".format(self.provider, exc))

        try:
            parsed = strict_json_loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            raise ClientError(
                "{} returned a non-JSON response: {}".format(self.provider, exc)
            )
        if not isinstance(parsed, dict):
            raise ClientError("{} returned a non-object JSON response".format(self.provider))
        return self.parse_response(parsed)


def _decode_http_error(exc: urllib.error.HTTPError) -> str:
    try:
        detail = exc.read().decode("utf-8", "replace")
    except OSError:
        return "<no response body>"
    detail = " ".join(detail.split())
    if len(detail) > 2000:
        detail = detail[:2000] + "..."
    return detail or "<empty response body>"


class AnthropicClient(_BaseHTTPClient):
    """Anthropic Messages API adapter."""

    provider = ANTHROPIC_PROVIDER
    key_env = ANTHROPIC_KEY_ENV

    def build_request(self, prompt: str) -> Tuple[str, Dict[str, str], Dict[str, Any]]:
        url = anthropic_endpoint(self.base_url)
        headers = {
            "content-type": "application/json",
            "x-api-key": self._api_key,
            "anthropic-version": ANTHROPIC_VERSION,
        }
        payload = {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "temperature": 0,
            "messages": [{"role": "user", "content": prompt}],
        }
        return url, headers, payload

    def parse_response(self, payload: Dict[str, Any]) -> str:
        content = payload.get("content")
        if not isinstance(content, list):
            raise ClientError("Anthropic response has no content array")
        texts = []  # type: List[str]
        for block in content:
            if isinstance(block, dict) and block.get("type") == "text":
                text = block.get("text")
                if isinstance(text, str):
                    texts.append(text)
        result = "".join(texts)
        if not result.strip():
            raise ClientError("Anthropic response contains no text content")
        return result


class OpenAIClient(_BaseHTTPClient):
    """OpenAI-compatible ``/v1/chat/completions`` adapter."""

    provider = OPENAI_PROVIDER
    key_env = OPENAI_KEY_ENV

    def build_request(self, prompt: str) -> Tuple[str, Dict[str, str], Dict[str, Any]]:
        url = openai_endpoint(self.base_url)
        headers = {
            "content-type": "application/json",
            "authorization": "Bearer {}".format(self._api_key),
        }
        payload = {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "temperature": 0,
            "messages": [{"role": "user", "content": prompt}],
        }
        return url, headers, payload

    def parse_response(self, payload: Dict[str, Any]) -> str:
        choices = payload.get("choices")
        if not isinstance(choices, list) or not choices:
            raise ClientError("OpenAI-compatible response has no choices")
        first = choices[0]
        if not isinstance(first, dict):
            raise ClientError("OpenAI-compatible response choice is malformed")
        message = first.get("message")
        if not isinstance(message, dict):
            raise ClientError("OpenAI-compatible response has no message")
        content = message.get("content")
        if not isinstance(content, str) or not content.strip():
            raise ClientError("OpenAI-compatible response contains no text content")
        return content


class FakeClient:
    """Deterministic in-memory client used by tests and offline experiments.

    A single string response is returned for every call.  A sequence is
    consumed one response per call, which lets tests exercise multi-step
    flows without touching the network.
    """

    def __init__(self, responses: Union[str, Sequence[str]]) -> None:
        if isinstance(responses, str):
            self._responses = [responses]
            self._repeat_last = True
        else:
            self._responses = list(responses)
            self._repeat_last = False
        if not self._responses:
            raise ValueError("FakeClient needs at least one response")
        self.prompts = []  # type: List[str]

    def complete(self, prompt: str) -> str:
        self.prompts.append(prompt)
        if not self._responses:
            raise ClientError("FakeClient has no responses left")
        if len(self._responses) == 1 and self._repeat_last:
            return self._responses[0]
        return self._responses.pop(0)


def call_client(client: Any, prompt: str) -> str:
    """Call an object with ``complete`` or a plain callable.

    This is the injection point that keeps the pipeline testable without a
    network connection.
    """
    if hasattr(client, "complete"):
        response = client.complete(prompt)
    elif callable(client):
        response = client(prompt)
    else:
        raise TypeError("client must expose complete(prompt) or be callable")
    if not isinstance(response, str):
        raise ClientError("LLM client returned a non-string response")
    return response


def config_from_env(env: Optional[Mapping[str, str]] = None) -> Dict[str, Optional[str]]:
    """Read provider/model/base-URL configuration from the environment."""
    source = os.environ if env is None else env
    provider = (source.get(PROVIDER_ENV) or "").strip().lower()
    if not provider:
        raise ConfigError(
            "{} must be set to 'anthropic' or 'openai'".format(PROVIDER_ENV)
        )
    if provider not in SUPPORTED_PROVIDERS:
        raise ConfigError(
            "unsupported {}={!r}; expected 'anthropic' or 'openai'".format(
                PROVIDER_ENV, provider
            )
        )
    model = (source.get(MODEL_ENV) or "").strip()
    if not model:
        raise ConfigError("{} must be set".format(MODEL_ENV))
    base_url = source.get(BASE_URL_ENV) or None
    return {"provider": provider, "model": model, "base_url": base_url}


def client_from_env(
    env: Optional[Mapping[str, str]] = None,
    timeout: float = 60.0,
    max_tokens: int = 8192,
) -> Union[AnthropicClient, OpenAIClient]:
    """Construct the configured provider adapter without performing a request."""
    config = config_from_env(env)
    source = os.environ if env is None else env
    provider = config["provider"]
    model = config["model"]  # type: str
    base_url = config["base_url"]
    if provider == ANTHROPIC_PROVIDER:
        return AnthropicClient(
            model,
            api_key=source.get(ANTHROPIC_KEY_ENV),
            base_url=base_url,
            timeout=timeout,
            max_tokens=max_tokens,
            env=source,
        )
    return OpenAIClient(
        model,
        api_key=source.get(OPENAI_KEY_ENV),
        base_url=base_url,
        timeout=timeout,
        max_tokens=max_tokens,
        env=source,
    )
