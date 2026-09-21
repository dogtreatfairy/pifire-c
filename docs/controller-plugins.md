# Writing a controller plugin

A controller turns the pit temperature and set point into a **feed ratio `u` in [0, 1]**: the fraction of each auger cycle the auger runs. Everything else — the cycle timing, `u_min`/`u_max` clamps, the absolute auger-on cap, lid-open pauses, safety — is owned by the daemon, so a plugin only has to do control math.

The ABI is `include/pifire/controller.h`, currently **version 2**. A plugin built against version 1 is refused with a logged reason, so rebuild it after updating; version 2 only adds the three gain-schedule fields described below, which a plugin may ignore. Plugins can be compiled into the daemon (add to `src/controllers/registry.c`) or built as shared objects and dropped into `/usr/lib/pifire/controllers/`. `plugins/example_controller/` is a complete, dependency-free example.

## Units and timing

* All temperatures in the ABI are **Celsius**. Options that are temperatures in the user's units (like a proportional band) arrive in the config JSON together with a `"_units": "F"|"C"` key — convert them once in `create()`/`configure()` (`pf_delta_to_c` in `pifire/common.h`).
* `update()` is called **once per cycle** (`in->cycle_time_s`, default 25 s) while in Hold. It is *not* called while the lid-open pause is active, during manual overrides, or while autotune is running — so integrators never wind up on phantom error.
* `reset(in)` is called on Hold entry, on a controller switch and when a fault is cleared. Contract: initialise state so that the first `update()` returns approximately `in->u_prev_applied` (bumpless transfer).

## Inputs (`pf_ctrl_in`)

| field | meaning |
|---|---|
| `now_s` | monotonic seconds |
| `pit_c`, `setpoint_c`, `ambient_c` | temperatures; `ambient_c` may be NaN |
| `u_prev_raw` / `u_prev_applied` | your last output / what the daemon actually ran after clamping |
| `u_ff` | learned steady-state feed for this set point and ambient (see `docs/safety.md`, "Learning") |
| `sched_PB_c`, `sched_Ti`, `sched_Td` | tuning autotune measured at this set point, interpolated between the entries in the tuning library; all zero when nothing has been measured. A PID-family plugin should prefer these over its configured values, because a pellet grill's process gain falls as it gets hotter and one fixed band does not suit 180 °F and 450 °F alike. See `docs/learning.md`. |
| `saturated` | −1 clamped at `u_min`, +1 at `u_max`, 0 free — use it for conditional integration |
| `cycle_time_s`, `u_min`, `u_max` | current cycle configuration |
| `target_reached`, `fan_on`, `fan_pct` | state hints |
| `hist` | ring buffer of the last 60 minutes at 1 Hz (`pf_history_at(h, i)`, oldest first) |

## Outputs

Return `u`. The daemon clamps it; if you return NaN/inf or a wild value three cycles in a row it swaps in the built-in PID and raises `E06_CONTROLLER_FAULT`. Fill `pf_ctrl_dbg` (p/i/d/ff/error…) — it is shown in the UI and published over MQTT.

## Options schema

`config_schema_json` is a JSON array of option descriptors; the settings UI renders it and stores values under `settings.controller.config.<id>`:

```json
[{"option_name":"PB","option_friendly_name":"Proportional Band","option_description":"...",
  "option_type":"float","option_default":60.0,"option_step":0.1,"units":"temp_delta"}]
```

`option_type` is `float` or `bool`; `"units":"temp_delta"` marks a temperature difference (converted when the user switches °F/°C).

## Persistence

`env->kv_put(env, key, json)` / `env->kv_get(env, key, out, n)` store JSON under a namespace private to your controller id (SQLite-backed). `env->log(level, tag, fmt, ...)` writes to the daemon log.

## Tuning hooks (optional)

* `apply_tuning(self, Ku, Pu, K, tau, theta)` — called when the user applies autotune (`Ku`, `Pu`) or the passive plant estimate (`K`, `tau`, `theta`). Zero means "not available".
* `episode_end(self, ep)` — summary at the end of a cook.

## Checklist

1. `abi = PF_CONTROLLER_ABI`, unique `id`, non-NULL `create/destroy/reset/update`.
2. Export `const pf_controller_ops *pf_controller_export(void)` from the `.so`.
3. Keep `update()` cheap (it runs on the control thread) and never block.
4. Test in the simulator: `pifired --sim --speed 20`, select your controller, watch the History page.
