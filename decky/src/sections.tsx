// Panel sections: the types and the plain functions that build the JSX of the quick-access panel (no hooks).
import type { Dispatch, ReactElement, SetStateAction } from "react";
import { ButtonItem, Focusable, PanelSection, PanelSectionRow, SliderField, ToggleField } from "@decky/ui";

export type Term = { key: string; eq: number };
export type SchemaItem = {
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
export type LayerInfo = { installed: boolean; manifest: string; library: string | null; bundled_available: boolean };
export type State = {
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

const LAUNCH_PLAIN = "VKIFRAME_ENABLE=1";
const LAUNCH_FULL = "VKIFRAME_ENABLE=1 %command%";
const LAUNCH_32BIT = "PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%";   // 32-bit games: Proton runs them inside a 64-bit process, so the 64-bit layer can load
const LAUNCH_DXVK_ONLY = 'DXVK_CONFIG="dxvk.enablePresentTiming = False" VKIFRAME_ENABLE=1 %command%';   // the same without WOW64 (not tested separately: the tested line is the one below)
const LAUNCH_DXVK = 'DXVK_CONFIG="dxvk.enablePresentTiming = False" PROTON_USE_WOW64=1 VKIFRAME_ENABLE=1 %command%';   // Proton - Experimental, game loads the layer but gets no extra frames: DXVK stops using present timing (a workaround, tested in one game)

// One launch option to copy: the whole line on a small font that wraps anywhere (the lines are long), a one-line note under it.
export function LaunchRow({ text, note, onCopy }: { text: string; note: string; onCopy: () => void }) {
  return (
    <PanelSectionRow>
      <ButtonItem layout="below" onClick={onCopy}>
        <span style={{ fontSize: 11, lineHeight: 1.25, whiteSpace: "normal", wordBreak: "break-all" }}>{text}</span>
        <div style={{ fontSize: 10, opacity: 0.75, marginTop: 2 }}>{note}</div>
      </ButtonItem>
    </PanelSectionRow>
  );
}

export function describeValue(item: SchemaItem, v: number): string {
  if ((item.key === "BASE_FPS_CAP" || item.key === "UI_MASK_THRESHOLD") && v === 0) return "off";
  const step = item.fine ?? item.step ?? 1;
  if (Number.isInteger(step)) return String(v);
  return v.toFixed(step < 0.01 ? 3 : 2);
}

// A greyed button with no action: it only gives the gamepad something to land on, so the panel can scroll down to the end of a text-only section.
export function EndMarker({ text }: { text: string }) {
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
export function Gap() {
  return <div style={{ height: 1, margin: "18px 16px", background: "rgba(255,255,255,0.15)" }} />;
}

// Section: layer install state and the live status line.
export function renderStatusSection(layer: LayerInfo, statusText: string, onInstall: () => void) {
  return (
    <PanelSection title="Layer and status">
      {/* Only while the layer is missing. Once it is installed the button is gone from the top, so a stray tap cannot reinstall it; Reinstall and Uninstall are at the very bottom of the panel. */}
      {!layer.installed && (
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={!layer.bundled_available}
            onClick={onInstall}
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
          <div style={{ marginTop: 4 }}>{statusText}</div>
        </Focusable>
      </PanelSectionRow>
      <EndMarker text="End of status" />
    </PanelSection>
  );
}

// Section: the launch options to copy.
export function renderLaunchSection(copy: (text: string) => void) {
  return (
    <PanelSection title="Launch options">
      <PanelSectionRow>
        <div style={{ fontSize: 11, opacity: 0.75 }}>
          Copy one into the game's launch options (Properties → General). The plugin never sets it. Already have options? Put VKIFRAME_ENABLE=1 before %command%.
        </div>
      </PanelSectionRow>
      <LaunchRow text={LAUNCH_PLAIN} note="Plain" onCopy={() => copy(LAUNCH_PLAIN)} />
      <LaunchRow text={LAUNCH_FULL} note="Plain with %command%" onCopy={() => copy(LAUNCH_FULL)} />
      <LaunchRow text={LAUNCH_32BIT} note="32-bit games (Proton WOW64; not every game works)" onCopy={() => copy(LAUNCH_32BIT)} />
      <LaunchRow text={LAUNCH_DXVK_ONLY} note="DXVK without present timing (not tested by itself)" onCopy={() => copy(LAUNCH_DXVK_ONLY)} />
      <LaunchRow text={LAUNCH_DXVK} note="DXVK without present timing + 32-bit" onCopy={() => copy(LAUNCH_DXVK)} />
      <PanelSectionRow>
        <div style={{ fontSize: 11, opacity: 0.75 }}>
          No extra frames (seen with Batman: Arkham Asylum on Proton - Experimental)? Try GE-Proton, or the last line (tested on one game). The DXVK-only line is not tested. They turn off present timing in DXVK; may change frame pacing. Remove if the game misbehaves.
        </div>
      </PanelSectionRow>
    </PanelSection>
  );
}

// Section: benchmark button and the last result.
export function renderBenchSection(state: State, gameRunning: boolean, benchRunning: boolean, benchLabel: string, onBench: () => void) {
  return (
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
          onClick={onBench}
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
  );
}

// Section: the change log tail.
export function renderLogSection(log: string[]) {
  return (
    <PanelSection title="Change log">
      <PanelSectionRow>
        <div style={{ fontSize: 10, fontFamily: "monospace", whiteSpace: "pre-wrap", wordBreak: "break-all", opacity: 0.85 }}>
          {log.length ? log.join("\n") : "Nothing logged yet."}
        </div>
      </PanelSectionRow>
      <EndMarker text="End of change log" />
    </PanelSection>
  );
}

// Section: the main settings and the fine-steps switch.
export function renderMainSection(schema: SchemaItem[], fine: boolean, setFine: (on: boolean) => void, renderItem: (item: SchemaItem) => ReactElement) {
  return (
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
  );
}

// The "needs restart" notice between the live and the restart-only settings.
export function renderRestartNotice() {
  return (
    <PanelSectionRow>
      <div style={{ marginTop: 12, padding: "6px 8px", borderLeft: "3px solid #e8a33d", background: "rgba(232, 163, 61, 0.12)" }}>
        <div style={{ fontSize: 13, fontWeight: 700, letterSpacing: 0.5 }}>⚠ NEEDS RESTART</div>
        <div style={{ fontSize: 11, opacity: 0.85, lineHeight: 1.3 }}>
          Read only when a game starts. Change them, then restart the game.
        </div>
      </div>
    </PanelSectionRow>
  );
}

// Section: the more settings, folded away until opened.
export function renderMoreSection(schema: SchemaItem[], showMore: boolean, setShowMore: Dispatch<SetStateAction<boolean>>, renderItem: (item: SchemaItem) => ReactElement, onReset: () => void) {
  return (
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
          {renderRestartNotice()}
          {schema.filter((i) => i.group === "more" && i.needsLayer === "restart").map(renderItem)}
        </>
      )}
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={onReset}>
          Reset all to defaults
        </ButtonItem>
      </PanelSectionRow>
    </PanelSection>
  );
}

// Section: shown while frame generation is off.
export function renderSettingsOff(onReset: () => void) {
  return (
    <PanelSection title="Settings">
      <PanelSectionRow>
        <div style={{ fontSize: 11, opacity: 0.75 }}>Turn Frame generation on to see the settings.</div>
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={onReset}>
          Reset all to defaults
        </ButtonItem>
      </PanelSectionRow>
    </PanelSection>
  );
}

// The two-step uninstall rows: ask first, then confirm or cancel.
export function renderUninstallRows(confirmUninstall: boolean, setConfirmUninstall: (on: boolean) => void, onUninstall: () => void) {
  return confirmUninstall ? (
    <>
      <PanelSectionRow>
        <div style={{ fontSize: 12, fontWeight: 600 }}>Remove the layer? Games run without frame generation until you install it again.</div>
      </PanelSectionRow>
      <PanelSectionRow>
        <ButtonItem layout="below" onClick={() => { setConfirmUninstall(false); onUninstall(); }}>
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
  );
}

// Section: reinstall and uninstall of the layer.
export function renderMaintenance(layer: LayerInfo, confirmUninstall: boolean, setConfirmUninstall: (on: boolean) => void, onReinstall: () => void, onUninstall: () => void) {
  return (
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
          onClick={onReinstall}
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
      {renderUninstallRows(confirmUninstall, setConfirmUninstall, onUninstall)}
    </PanelSection>
  );
}

export function isGreyed(item: SchemaItem, values: Record<string, number>, all: SchemaItem[]): boolean {
  const val = (k: string) => values[k] ?? all.find((i) => i.key === k)?.default ?? 0;
  if (item.group === "more" && !val("EXPERIMENTAL")) return true;      // experimental settings: locked until Experimental is on
  return (item.greyWhen ?? []).some((group) => group.every((t) => val(t.key) === t.eq));
}

// The description under a setting: its value, text, notes and why it is greyed.
export function renderNote(item: SchemaItem, value: number, grey: boolean, state: State) {
  const { values } = state;
  return (
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
}

// One setting row: a toggle or a slider, greyed out while the settings it depends on say so.
export function renderSetting(item: SchemaItem, state: State, fine: boolean, change: (key: string, value: number) => void, fineShortcut: Record<string, unknown>) {
  const { schema, values } = state;
  const grey = isGreyed(item, values, schema);
  const value = values[item.key] ?? item.default;
  const note = renderNote(item, value, grey, state);
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
}

// The settings area: all sections while frame generation is on, a short hint while it is off.
export function renderSettings(
  schema: SchemaItem[], values: Record<string, number>, fine: boolean, setFine: (on: boolean) => void,
  showMore: boolean, setShowMore: Dispatch<SetStateAction<boolean>>, renderItem: (item: SchemaItem) => ReactElement, onReset: () => void,
) {
  return values.ENABLED ? (
    <>
      <Gap />
      {renderMainSection(schema, fine, setFine, renderItem)}

      <Gap />
      {renderMoreSection(schema, showMore, setShowMore, renderItem, onReset)}
    </>
  ) : (
    <>
      <Gap />
      {renderSettingsOff(onReset)}
    </>
  );
}
