#include "ghostclaw/config/schema.hpp"

namespace ghostclaw::config {

std::string json_schema() {
  return R"JSON({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://ghostclaw.dev/schemas/config.schema.json",
  "title": "GhostClaw Config",
  "type": "object",
  "additionalProperties": true,
  "properties": {
    "api_key": {"type": "string"},
    "default_provider": {"type": "string"},
    "default_model": {"type": "string"},
    "default_temperature": {"type": "number", "minimum": 0.0, "maximum": 2.0},
    "memory": {
      "type": "object",
      "properties": {
        "backend": {"type": "string"},
        "auto_save": {"type": "boolean"},
        "embedding_provider": {"type": "string"},
        "embedding_model": {"type": "string"},
        "embedding_dimensions": {"type": "integer", "minimum": 1},
        "embedding_cache_size": {"type": "integer", "minimum": 1},
        "vector_weight": {"type": "number", "minimum": 0.0, "maximum": 1.0},
        "keyword_weight": {"type": "number", "minimum": 0.0, "maximum": 1.0}
      }
    },
    "gateway": {
      "type": "object",
      "properties": {
        "host": {"type": "string"},
        "port": {"type": "integer", "minimum": 1, "maximum": 65535},
        "websocket_enabled": {"type": "boolean"},
        "websocket_port": {"type": "integer", "minimum": 0, "maximum": 65535},
        "websocket_host": {"type": "string"},
        "websocket_tls_enabled": {"type": "boolean"},
        "websocket_tls_cert_file": {"type": "string"},
        "websocket_tls_key_file": {"type": "string"}
      }
    },
    "autonomy": {
      "type": "object",
      "properties": {
        "level": {"type": "string", "enum": ["readonly", "supervised", "full"]},
        "workspace_only": {"type": "boolean"},
        "allowed_commands": {"type": "array", "items": {"type": "string"}},
        "forbidden_paths": {"type": "array", "items": {"type": "string"}},
        "max_actions_per_hour": {"type": "integer", "minimum": 1},
        "max_cost_per_day_cents": {"type": "integer", "minimum": 0}
      }
    },
    "channels": {
      "type": "object",
      "additionalProperties": true,
      "properties": {
        "telegram": {"type": "object"},
        "discord": {"type": "object"},
        "slack": {"type": "object"},
        "matrix": {"type": "object"},
        "imessage": {"type": "object"},
        "whatsapp": {"type": "object"},
        "webhook": {"type": "object"}
      }
    },
    "tunnel": {
      "type": "object",
      "properties": {
        "provider": {"type": "string", "enum": ["none", "cloudflare", "ngrok", "tailscale", "custom"]}
      }
    },
    "reliability": {
      "type": "object",
      "properties": {
        "provider_retries": {"type": "integer", "minimum": 0},
        "provider_backoff_ms": {"type": "integer", "minimum": 0},
        "fallback_providers": {"type": "array", "items": {"type": "string"}}
      }
    },
    "heartbeat": {
      "type": "object",
      "properties": {
        "enabled": {"type": "boolean"},
        "interval_minutes": {"type": "integer", "minimum": 1},
        "tasks_file": {"type": "string"}
      }
    },
    "tools": {
      "type": "object",
      "properties": {
        "profile": {"type": "string"},
        "allow": {"type": "object"}
      }
    },
    "calendar": {"type": "object"},
    "email": {"type": "object"},
    "reminders": {"type": "object"},
    "web_search": {"type": "object"},
    "composio": {"type": "object"},
    "identity": {"type": "object"},
    "secrets": {"type": "object"},
    "multi": {"type": "object"},
    "daemon": {"type": "object"},
    "mcp": {"type": "object"},
    "google": {"type": "object"},
    "conway": {
      "type": "object",
      "properties": {
        "enabled": {"type": "boolean"},
        "api_key": {"type": "string"},
        "wallet_path": {"type": "string"},
        "config_path": {"type": "string"},
        "api_url": {"type": "string"},
        "default_region": {"type": "string", "enum": ["eu-north", "us-east"]},
        "survival_monitoring": {"type": "boolean"},
        "low_compute_threshold_usd": {"type": "number", "minimum": 0.0},
        "critical_threshold_usd": {"type": "number", "minimum": 0.0}
      }
    },
    "soul": {
      "type": "object",
      "properties": {
        "enabled": {"type": "boolean"},
        "path": {"type": "string"},
        "git_versioned": {"type": "boolean"},
        "protected_sections": {"type": "array", "items": {"type": "string"}},
        "max_reflections": {"type": "integer", "minimum": 1}
      }
    }
  }
})JSON";
}

} // namespace ghostclaw::config
