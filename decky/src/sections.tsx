// Panel sections: the types and the plain functions that build the JSX of the quick-access panel (no hooks).
import { ButtonItem, Focusable, PanelSectionRow } from "@decky/ui";

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
