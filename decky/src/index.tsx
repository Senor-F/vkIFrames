import {
  ButtonItem,
  Focusable,
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin, toaster } from "@decky/api";
import { useEffect, useRef, useState } from "react";
import { FaFilm } from "react-icons/fa";

type Term = { key: string; eq: number };
type SchemaItem = {
  key: string;
  label: string;
  type: "toggle" | "slider";
  default: number;
  min?: number;
  max?: number;
  step?: number;          // normal slider step
  fine?: number;          // finer step, used while "fine steps" is on
  desc?: string;          // one or two sentences for the user
  group?: "switch" | "main" | "gate" | "more";
  needsLayer?: "live" | "restart";   // live: applied within a second; restart: read when a game starts
  greyWhen?: Term[][];    // greyed out when any group has all its terms true
  greyWhy?: string;
};
type LayerInfo = { installed: boolean; manifest: string; library: string | null; bundled_available: boolean };
type State = {
  schema: SchemaItem[];
  values: Record<string, number>;
  layer: LayerInfo;
  live_conf: string;
  refresh_hz: number;
  refresh_src: string;
  status: Record<string, unknown> | null;
  bench: { status: string | null; reason: string; age_s: number; layer_cost_mean_ms: number | null; gpu_share: number | null } | null;
  bench_file: string;
};

const getState = callable<[], State>("get_state");
const setValue = callable<[key: string, value: number], Record<string, number>>("set_value");
const resetValues = callable<[], Record<string, number>>("reset_values");
const installLayer = callable<[lib_path: string], LayerInfo>("install_layer");
const uninstallLayer = callable<[], LayerInfo>("uninstall_layer");
const getLog = callable<[lines: number], string[]>("get_log");
const startBench = callable<[], { token: number }>("start_bench");

const LAUNCH_PLAIN = "VKIFRAME_ENABLE=1";
const LAUNCH_FULL = "VKIFRAME_ENABLE=1 %command%";
const LAUNCH_32BIT = "PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%";   // 32-bit games: Proton runs them inside a 64-bit process, so the 64-bit layer can load
const LAUNCH_DXVK = 'DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%';   // Proton - Experimental, game loads the layer but gets no extra frames: DXVK stops using present timing (a workaround, tested in one game)

function describeValue(item: SchemaItem, v: number): string {
  if ((item.key === "BASE_FPS_CAP" || item.key === "UI_MASK_THRESHOLD") && v === 0) return "off";
  const step = item.fine ?? item.step ?? 1;
  if (Number.isInteger(step)) return String(v);
  return v.toFixed(step < 0.01 ? 3 : 2);
}

function isGreyed(item: SchemaItem, values: Record<string, number>, all: SchemaItem[]): boolean {
  const val = (k: string) => values[k] ?? all.find((i) => i.key === k)?.default ?? 0;
  if (item.group === "more" && !val("EXPERIMENTAL")) return true;      // experimental settings: locked until Experimental is on
  return (item.greyWhen ?? []).some((group) => group.every((t) => val(t.key) === t.eq));
}

// Steam's overlay is a browser: try the clipboard API, then the old textarea route.
async function copyText(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (_) {
    /* fall through */
  }
  try {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.position = "fixed";
    ta.style.opacity = "0";
    document.body.appendChild(ta);
    ta.focus();
    ta.select();
    const ok = document.execCommand("copy");
    document.body.removeChild(ta);
    return ok;
  } catch (_) {
    return false;
  }
}

function statusLine(status: Record<string, unknown> | null): string {
  if (!status) return "No status yet. Start a game with VKIFRAME_ENABLE=1 in its launch options.";
  const age = Number(status["age_s"] ?? 0);
  if (age > 5) return "No recent status: no game running, or the layer is not loaded.";
  const mode = String(status["mode"] ?? "?");
  const k = Number(status["k"] ?? 0);
  const modeText = mode === "passthru" ? "pass-through (frame holding off)" : mode;      // the layer cannot hold frames of this game: nothing is generated
  return `${status["native_fps"]} fps from the game → ${status["output_fps"]} fps shown | ${k} generated per game frame | mode: ${modeText}`;
}

function Content() {
  const [state, setState] = useState<State | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [log, setLog] = useState<string[]>([]);
  const [fine, setFine] = useState(false);
  const [showMore, setShowMore] = useState(false);
  const [confirmUninstall, setConfirmUninstall] = useState(false);   // Uninstall needs a second tap; it asks again after 8 s
  useEffect(() => {
    if (!confirmUninstall) return;
    const t = setTimeout(() => setConfirmUninstall(false), 8000);
    return () => clearTimeout(t);
  }, [confirmUninstall]);
  const timers = useRef<Record<string, ReturnType<typeof setTimeout>>>({});

  const refresh = async () => {
    try {
      setState(await getState());
      setError(null);
      setLog(await getLog(8).catch(() => []));
    } catch (e) {
      setError(String(e));
    }
  };

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 2000); // pick up the layer's status file
    return () => clearInterval(t);
  }, []);

  // Update the screen at once, write to the backend once the slider has settled (150 ms).
  const change = (key: string, value: number) => {
    setState((s) => (s ? { ...s, values: { ...s.values, [key]: value } } : s));
    clearTimeout(timers.current[key]);
    timers.current[key] = setTimeout(() => {
      setValue(key, value)
        .then(() => refresh())
        .catch((e) => toaster.toast({ title: "vkIFrames", body: String(e) }));
    }, 150);
  };

  const act = async (fn: () => Promise<unknown>, ok: string) => {
    try {
      await fn();
      toaster.toast({ title: "vkIFrames", body: ok });
    } catch (e) {
      toaster.toast({ title: "vkIFrames", body: String(e) });
    }
    refresh();
  };

  const copy = async (text: string) => {
    const ok = await copyText(text);
    toaster.toast({ title: "vkIFrames", body: ok ? `Copied: ${text}` : `Could not copy. Type it: ${text}` });
  };

  if (error) return <PanelSection title="vkIFrames"><PanelSectionRow>{error}</PanelSectionRow></PanelSection>;
  if (!state) return <PanelSection title="vkIFrames"><PanelSectionRow>Loading…</PanelSectionRow></PanelSection>;

  const { layer, schema, values, status } = state;
  const gameRunning = !!status && Number(status["age_s"] ?? 99) <= 5;
  const benchLabel = String(status?.["bench"] ?? "");
  const benchRunning = gameRunning && benchLabel !== "" && benchLabel !== "done" && benchLabel !== "aborted";

  // A slider takes the secondary gamepad button as a shortcut for fine steps; the legend at the bottom of the screen names the button.
  const fineShortcut = {
    onSecondaryButton: () => setFine((f) => !f),
    onSecondaryActionDescription: fine ? "Coarse steps" : "Fine steps",
  } as Record<string, unknown>;

  const renderItem = (item: SchemaItem) => {
    const grey = isGreyed(item, values, schema);
    const value = values[item.key] ?? item.default;
    const note = (
      <div style={{ fontSize: 11, opacity: grey ? 0.55 : 0.75, lineHeight: 1.3 }}>
        {item.type === "slider" && <div style={{ fontWeight: 600 }}>{describeValue(item, value)}</div>}
        {item.desc && <div>{item.desc}</div>}
        {item.key === "TARGET_FPS" && (
          <div>
            {state?.refresh_hz ? `Screen: ${Math.round(state.refresh_hz)} Hz (${state.refresh_src}). Slider up to ${item.max}.` : `Screen refresh rate not detected yet. Slider up to ${item.max}.`}
          </div>
        )}
        {item.key === "MAX_SYNTHETIC" && Number(state?.status?.["age_s"] ?? 99) <= 5 && Number(state?.status?.["synth_capacity"] ?? 0) > 0 && value > Number(state?.status?.["synth_capacity"]) && (
          <div style={{ fontStyle: "italic" }}>The running game was set up for at most {String(state?.status?.["synth_capacity"])}. Restart it to use more.</div>
        )}
        {grey && (item.group === "more" && !values.EXPERIMENTAL
          ? <div style={{ fontStyle: "italic" }}>Greyed out: turn on Experimental to change this.</div>
          : item.greyWhy && <div style={{ fontStyle: "italic" }}>Greyed out: {item.greyWhy}</div>)}
      </div>
    );
    return (
      <PanelSectionRow key={item.key}>
        {/* Greyed rows stay focusable (not `disabled`) so the gamepad can still land on them and the panel scrolls to them; changes are ignored while grey. */}
        <div style={grey ? { opacity: 0.45 } : undefined}>
        {item.type === "toggle" ? (
          <ToggleField
            label={item.label}
            checked={!!value}
            description={note}
            onChange={(on: boolean) => { if (!grey) change(item.key, on ? 1 : 0); }}
          />
        ) : (
          <SliderField
            label={item.label}
            value={value}
            min={item.min}
            max={item.max}
            step={fine && item.fine ? item.fine : item.step}
            showValue
            valueSuffix={item.key.endsWith("FPS") || item.key.endsWith("CAP") ? " fps" : undefined}
            onChange={(v: number) => { if (!grey) change(item.key, v); }}
            description={note}
            {...fineShortcut}
          />
        )}
        </div>
      </PanelSectionRow>
    );
  };

  return (
    <>
      <PanelSection title="Layer and status">
        {/* Only while the layer is missing. Once it is installed the button is gone from the top, so a stray tap cannot reinstall it; Reinstall and Uninstall are at the very bottom of the panel. */}
        {!layer.installed && (
          <PanelSectionRow>
            <ButtonItem
              layout="below"
              disabled={!layer.bundled_available}
              onClick={() => act(() => installLayer(""), "Layer installed")}
            >
              Install layer
            </ButtonItem>
          </PanelSectionRow>
        )}
        {!layer.installed && !layer.bundled_available && (
          <PanelSectionRow>
            <div style={{ fontSize: 11, opacity: 0.7 }}>
              No library in the plugin's bin/ folder (scripts/link-build.sh points at a local build).
            </div>
          </PanelSectionRow>
        )}
        <PanelSectionRow>
          <Focusable style={{ fontSize: 12, wordBreak: "break-word" }} onActivate={() => {}}>
            <div style={{ fontWeight: 600 }}>{layer.installed ? "Layer installed" : "Layer not installed"}</div>
            {layer.library && <div style={{ fontSize: 11, opacity: 0.7, wordBreak: "break-all" }}>{layer.library}</div>}
            <div style={{ marginTop: 4 }}>{statusLine(status)}</div>
          </Focusable>
        </PanelSectionRow>
        <EndMarker text="End of status" />
      </PanelSection>
      <Gap />

      <PanelSection title="Launch options">
        <PanelSectionRow>
          <div style={{ fontSize: 11, opacity: 0.75 }}>
            Put this in the game's launch options (Properties → General). The plugin does not set it for you. Settings are changed here, not there.
          </div>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => copy(LAUNCH_PLAIN)}>
            Copy: {LAUNCH_PLAIN}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => copy(LAUNCH_FULL)}>
            Copy: {LAUNCH_FULL}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => copy(LAUNCH_32BIT)}>
            Copy for 32-bit games: {LAUNCH_32BIT}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <div style={{ fontSize: 11, opacity: 0.75 }}>
            32-bit games need the third one: the layer is 64-bit, and PROTON_USE_WOW64=1 runs the game in a 64-bit process (Proton 10 or Experimental). Tested in one game only (Tomb Raider (2013)); not every 32-bit game works.
          </div>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => copy(LAUNCH_DXVK)}>
            Copy if no extra frames appear (Proton - Experimental): {LAUNCH_DXVK}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <div style={{ fontSize: 11, opacity: 0.75 }}>
            If the layer loads but shows no extra frames (seen with Batman: Arkham Asylum on Proton - Experimental), try GE-Proton, or this launch option. It disables present timing in DXVK; tested on one game, may change the game's frame pacing. Remove it if the game misbehaves.
          </div>
        </PanelSectionRow>
        <PanelSectionRow>
          <div style={{ fontSize: 11, opacity: 0.75 }}>
            No other launch options? Copy the second one. Already have some, e.g. MANGOHUD=1 %command%? Copy the first and put it before %command%: MANGOHUD=1 VKIFRAME_ENABLE=1 %command%
          </div>
        </PanelSectionRow>
      </PanelSection>
      <Gap />

      <PanelSection title="Frame generation">
        {schema.filter((i) => i.group === "switch").map(renderItem)}
      </PanelSection>

      {values.ENABLED ? (
        <>
          <Gap />
          <PanelSection title="Main settings (applies while playing)">
            <PanelSectionRow>
              <ToggleField
                label="Fine slider steps"
                checked={fine}
                onChange={(on: boolean) => setFine(on)}
                description={
                  <div style={{ fontSize: 11, opacity: 0.75 }}>
                    Smaller slider steps. With a slider selected, the button named at the bottom of the screen switches it too.
                  </div>
                }
              />
            </PanelSectionRow>
            {schema.filter((i) => i.group === "main").map(renderItem)}
          </PanelSection>

          <Gap />
          <PanelSection title="More settings">
            <PanelSectionRow>
              <ButtonItem layout="below" onClick={() => setShowMore((v) => !v)}>
                {showMore ? "▾ Hide more settings" : "▸ Show more settings"}
              </ButtonItem>
            </PanelSectionRow>
            {showMore && (
              <>
                <PanelSectionRow>
                  <div style={{ fontSize: 11, opacity: 0.85 }}>
                    Untested: these can add latency, glitches or crashes. They can only be changed while Experimental is on.
                  </div>
                </PanelSectionRow>
                {schema.filter((i) => i.group === "gate").map(renderItem)}
                {schema.filter((i) => i.group === "more" && (i.needsLayer ?? "live") === "live").map(renderItem)}
                <PanelSectionRow>
                  <div style={{ marginTop: 12, padding: "6px 8px", borderLeft: "3px solid #e8a33d", background: "rgba(232, 163, 61, 0.12)" }}>
                    <div style={{ fontSize: 13, fontWeight: 700, letterSpacing: 0.5 }}>⚠ NEEDS RESTART</div>
                    <div style={{ fontSize: 11, opacity: 0.85, lineHeight: 1.3 }}>
                      Read only when a game starts. Change them, then restart the game.
                    </div>
                  </div>
                </PanelSectionRow>
                {schema.filter((i) => i.group === "more" && i.needsLayer === "restart").map(renderItem)}
              </>
            )}
            <PanelSectionRow>
              <ButtonItem layout="below" onClick={() => act(() => resetValues(), "Defaults restored")}>
                Reset all to defaults
              </ButtonItem>
            </PanelSectionRow>
          </PanelSection>
        </>
      ) : (
        <>
          <Gap />
          <PanelSection title="Settings">
            <PanelSectionRow>
              <div style={{ fontSize: 11, opacity: 0.75 }}>Turn Frame generation on to see the settings.</div>
            </PanelSectionRow>
            <PanelSectionRow>
              <ButtonItem layout="below" onClick={() => act(() => resetValues(), "Defaults restored")}>
                Reset all to defaults
              </ButtonItem>
            </PanelSectionRow>
          </PanelSection>
        </>
      )}
      <Gap />

      <PanelSection title="Benchmark">
        <PanelSectionRow>
          <div style={{ fontSize: 11, opacity: 0.75 }}>
            Measures what the layer costs: 30 s warm-up, then 6 x (60 s off, 60 s on), about 13 min. Stay in one demanding scene; it stops if you change a setting or leave the game. Result: bench.json in the vkiframes config folder.
          </div>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={!gameRunning || benchRunning}
            onClick={() => act(() => startBench(), "Benchmark requested")}
          >
            {benchRunning ? "Benchmark running: " + benchLabel : "Run benchmark (13 min)"}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <Focusable style={{ fontSize: 12 }} onActivate={() => {}}>
            {state.bench
              ? state.bench.status === "complete"
                ? `Last run: layer costs ${state.bench.layer_cost_mean_ms} ms per game frame, ${Math.round((state.bench.gpu_share ?? 0) * 100)} % of the GPU (${Math.round(state.bench.age_s / 60)} min ago)`
                : `Last run stopped: ${state.bench.reason}`
              : "No benchmark run yet."}
          </Focusable>
        </PanelSectionRow>
        <EndMarker text="End of benchmark" />
      </PanelSection>
      <Gap />

      <PanelSection title="Change log">
        <PanelSectionRow>
          <div style={{ fontSize: 10, fontFamily: "monospace", whiteSpace: "pre-wrap", wordBreak: "break-all", opacity: 0.85 }}>
            {log.length ? log.join("\n") : "Nothing logged yet."}
          </div>
        </PanelSectionRow>
        <EndMarker text="End of change log" />
      </PanelSection>

      {layer.installed && (
        <>
          <Gap />
          <PanelSection title="Layer maintenance">
            <PanelSectionRow>
              <div style={{ fontSize: 11, opacity: 0.75 }}>
                Reinstall copies this plugin's layer over the installed one (after a plugin update). Uninstall removes the layer; games run without it until you install it again.
              </div>
            </PanelSectionRow>
            <PanelSectionRow>
              <ButtonItem
                layout="below"
                disabled={!layer.bundled_available}
                onClick={() => act(() => installLayer(""), "Layer reinstalled")}
              >
                Reinstall layer
              </ButtonItem>
            </PanelSectionRow>
            {!layer.bundled_available && (
              <PanelSectionRow>
                <div style={{ fontSize: 11, opacity: 0.7 }}>
                  No library in the plugin's bin/ folder, so it cannot be reinstalled (scripts/link-build.sh points at a local build).
                </div>
              </PanelSectionRow>
            )}
            {confirmUninstall ? (
              <>
                <PanelSectionRow>
                  <div style={{ fontSize: 12, fontWeight: 600 }}>Remove the layer? Games run without frame generation until you install it again.</div>
                </PanelSectionRow>
                <PanelSectionRow>
                  <ButtonItem layout="below" onClick={() => { setConfirmUninstall(false); act(() => uninstallLayer(), "Layer removed"); }}>
                    Yes, uninstall the layer
                  </ButtonItem>
                </PanelSectionRow>
                <PanelSectionRow>
                  <ButtonItem layout="below" onClick={() => setConfirmUninstall(false)}>
                    Cancel
                  </ButtonItem>
                </PanelSectionRow>
              </>
            ) : (
              <PanelSectionRow>
                <ButtonItem layout="below" onClick={() => setConfirmUninstall(true)}>
                  Uninstall layer…
                </ButtonItem>
              </PanelSectionRow>
            )}
          </PanelSection>
        </>
      )}
    </>
  );
}

// A greyed button with no action: it only gives the gamepad something to land on, so the panel can scroll down to the end of a text-only section.
function EndMarker({ text }: { text: string }) {
  return (
    <PanelSectionRow>
      <Focusable
        onActivate={() => {}}
        style={{
          opacity: 0.4,
          minHeight: 0,
          height: 20,
          lineHeight: "20px",
          padding: "0 10px",
          fontSize: 10,
          textAlign: "center",
          borderRadius: 3,
          background: "rgba(255,255,255,0.08)",
        }}
      >
        {text}
      </Focusable>
    </PanelSectionRow>
  );
}

// Blank space plus a thin line between sections, so one section's tooltip text is not mistaken for the next one's.
function Gap() {
  return <div style={{ height: 1, margin: "18px 16px", background: "rgba(255,255,255,0.15)" }} />;
}

export default definePlugin(() => ({
  name: "vkIFrames",
  titleView: <div className={staticClasses.Title}>vkIFrames</div>,
  content: <Content />,
  icon: <FaFilm />,
  onDismount() {},
}));
