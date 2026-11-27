// Quick-access panel UI (React).
import {
  PanelSection,
  PanelSectionRow,
  SliderField,
  ToggleField,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin, toaster } from "@decky/api";
import { useEffect, useRef, useState } from "react";
import { FaFilm } from "react-icons/fa";
import { Gap, describeValue, renderBenchSection, renderLaunchSection, renderLogSection, renderMainSection, renderMaintenance, renderMoreSection, renderSettingsOff, renderStatusSection } from "./sections";
import type { LayerInfo, SchemaItem, State } from "./sections";

const getState = callable<[], State>("get_state");
const setValue = callable<[key: string, value: number], Record<string, number>>("set_value");
const resetValues = callable<[], Record<string, number>>("reset_values");
const installLayer = callable<[lib_path: string], LayerInfo>("install_layer");
const uninstallLayer = callable<[], LayerInfo>("uninstall_layer");
const getLog = callable<[lines: number], string[]>("get_log");
const startBench = callable<[], { token: number }>("start_bench");

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
      {renderStatusSection(layer, statusLine(status), () => act(() => installLayer(""), "Layer installed"))}
      <Gap />

      {renderLaunchSection(copy)}
      <Gap />

      <PanelSection title="Frame generation">
        {schema.filter((i) => i.group === "switch").map(renderItem)}
      </PanelSection>

      {values.ENABLED ? (
        <>
          <Gap />
          {renderMainSection(schema, fine, setFine, renderItem)}

          <Gap />
          {renderMoreSection(schema, showMore, setShowMore, renderItem, () => act(() => resetValues(), "Defaults restored"))}
        </>
      ) : (
        <>
          <Gap />
          {renderSettingsOff(() => act(() => resetValues(), "Defaults restored"))}
        </>
      )}
      <Gap />

      {renderBenchSection(state, gameRunning, benchRunning, benchLabel, () => act(() => startBench(), "Benchmark requested"))}
      <Gap />

      {renderLogSection(log)}

      {layer.installed && (
        <>
          <Gap />
          {renderMaintenance(layer, confirmUninstall, setConfirmUninstall, () => act(() => installLayer(""), "Layer reinstalled"), () => act(() => uninstallLayer(), "Layer removed"))}
        </>
      )}
    </>
  );
}


export default definePlugin(() => ({
  name: "vkIFrames",
  titleView: <div className={staticClasses.Title}>vkIFrames</div>,
  content: <Content />,
  icon: <FaFilm />,
  onDismount() {},
}));
