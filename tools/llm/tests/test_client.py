"""Tests for provider adapters and JSON-response extraction (no network)."""

import sys
import unittest
from pathlib import Path

_REPO_ROOT = str(Path(__file__).resolve().parents[3])
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from tools.llm.client import (  # noqa: E402
    ANTHROPIC_VERSION,
    AnthropicClient,
    ClientError,
    ConfigError,
    FakeClient,
    OpenAIClient,
    ResponseParseError,
    anthropic_endpoint,
    call_client,
    client_from_env,
    extract_json_object,
    openai_endpoint,
    strict_json_loads,
)


class ExtractJsonObjectTests(unittest.TestCase):
    def test_plain_object(self):
        self.assertEqual(extract_json_object('{"a": 1}'), {"a": 1})

    def test_trailing_prose_and_leading_prose(self):
        text = 'Here is the answer:\n{"tasks": [{"name": "A"}]}\nThanks!'
        self.assertEqual(extract_json_object(text), {"tasks": [{"name": "A"}]})

    def test_markdown_fences(self):
        text = '```json\n{"tasks": [{"name": "A", "source": "int main(void){return 0;}"}]}\n```'
        self.assertEqual(extract_json_object(text)["tasks"][0]["name"], "A")

    def test_braces_inside_strings(self):
        text = 'prefix {"a": {"b": "}{"}, "c": "}"} suffix'
        self.assertEqual(extract_json_object(text), {"a": {"b": "}{"}, "c": "}"})

    def test_first_object_wins(self):
        text = '{"first": 1} then {"second": 2}'
        self.assertEqual(extract_json_object(text), {"first": 1})

    def test_no_object(self):
        with self.assertRaises(ResponseParseError):
            extract_json_object("no json here")

    def test_duplicate_keys_are_rejected(self):
        with self.assertRaises(ResponseParseError):
            extract_json_object('{"a": 1, "a": 2}')

    def test_nonstandard_constants_are_rejected(self):
        with self.assertRaises(ResponseParseError):
            extract_json_object('{"a": NaN}')

    def test_strict_json_loads(self):
        with self.assertRaises(ValueError):
            strict_json_loads('{"a": 1, "a": 2}')


class FakeClientTests(unittest.TestCase):
    def test_single_response_repeats_and_prompts_are_recorded(self):
        client = FakeClient('{"tasks": []}')
        self.assertEqual(client.complete("one"), '{"tasks": []}')
        self.assertEqual(client.complete("two"), '{"tasks": []}')
        self.assertEqual(client.prompts, ["one", "two"])

    def test_sequence_is_consumed(self):
        client = FakeClient(["first", "second"])
        self.assertEqual(client.complete("p"), "first")
        self.assertEqual(client.complete("p"), "second")
        with self.assertRaises(ClientError):
            client.complete("p")

    def test_callable_injection(self):
        self.assertEqual(call_client(lambda prompt: prompt.upper(), "hi"), "HI")

    def test_bad_callable_result(self):
        with self.assertRaises(ClientError):
            call_client(lambda prompt: 42, "hi")


class EnvironmentConfigTests(unittest.TestCase):
    def _env(self, **overrides):
        env = {
            "PTPN_LLM_PROVIDER": "anthropic",
            "PTPN_LLM_MODEL": "test-model",
            "ANTHROPIC_API_KEY": "super-secret-key",
        }
        env.update(overrides)
        return env

    def test_anthropic_request_shape_and_secret_redaction(self):
        client = client_from_env(self._env())
        self.assertIsInstance(client, AnthropicClient)
        self.assertNotIn("super-secret-key", repr(client))

        url, headers, payload = client.build_request("hello")
        self.assertEqual(url, "https://api.anthropic.com/v1/messages")
        self.assertEqual(headers["x-api-key"], "super-secret-key")
        self.assertEqual(headers["anthropic-version"], ANTHROPIC_VERSION)
        self.assertEqual(payload["model"], "test-model")
        self.assertEqual(payload["messages"], [{"role": "user", "content": "hello"}])

    def test_openai_request_shape(self):
        env = {
            "PTPN_LLM_PROVIDER": "openai",
            "PTPN_LLM_MODEL": "test-model",
            "OPENAI_API_KEY": "other-secret",
        }
        client = client_from_env(env)
        self.assertIsInstance(client, OpenAIClient)
        self.assertNotIn("other-secret", repr(client))
        url, headers, payload = client.build_request("hello")
        self.assertEqual(url, "https://api.openai.com/v1/chat/completions")
        self.assertEqual(headers["authorization"], "Bearer other-secret")
        self.assertEqual(payload["messages"][0]["content"], "hello")

    def test_missing_provider(self):
        with self.assertRaises(ConfigError):
            client_from_env({"PTPN_LLM_MODEL": "m", "ANTHROPIC_API_KEY": "k"})

    def test_unknown_provider(self):
        with self.assertRaises(ConfigError):
            client_from_env(self._env(PTPN_LLM_PROVIDER="other"))

    def test_missing_key_names_variable_without_value(self):
        env = self._env()
        del env["ANTHROPIC_API_KEY"]
        with self.assertRaises(ConfigError) as raised:
            client_from_env(env)
        self.assertIn("ANTHROPIC_API_KEY", str(raised.exception))

    def test_missing_model(self):
        env = self._env()
        del env["PTPN_LLM_MODEL"]
        with self.assertRaises(ConfigError):
            client_from_env(env)


class EndpointTests(unittest.TestCase):
    def test_anthropic_endpoint_forms(self):
        self.assertEqual(
            anthropic_endpoint(None), "https://api.anthropic.com/v1/messages"
        )
        self.assertEqual(
            anthropic_endpoint("https://proxy.example/api"),
            "https://proxy.example/api/v1/messages",
        )
        self.assertEqual(
            anthropic_endpoint("https://proxy.example/v1/messages"),
            "https://proxy.example/v1/messages",
        )

    def test_openai_endpoint_forms(self):
        self.assertEqual(
            openai_endpoint(None), "https://api.openai.com/v1/chat/completions"
        )
        self.assertEqual(
            openai_endpoint("http://localhost:8000"),
            "http://localhost:8000/v1/chat/completions",
        )
        self.assertEqual(
            openai_endpoint("http://localhost:8000/v1"),
            "http://localhost:8000/v1/chat/completions",
        )
        self.assertEqual(
            openai_endpoint("http://localhost:8000/v1/chat/completions"),
            "http://localhost:8000/v1/chat/completions",
        )

    def test_invalid_base_url(self):
        with self.assertRaises(ConfigError):
            openai_endpoint("ftp://example.com")

    def test_base_url_with_embedded_credentials_is_rejected(self):
        with self.assertRaises(ConfigError):
            openai_endpoint("https://user:password@example.com")


if __name__ == "__main__":
    unittest.main()
