import type { RulesetView } from "@accounts/types";
import { defaultRuleset } from "@common/rulesets";
import Typography from "@mui/material/Typography";

import { CodeCopy, PublicRow, PublishedRow } from "./rulesets_page";
import styles from "./rulesets_page.module.css";

export default {
  title: "pages/Rulesets/Rows",
  argTypes: {},
};

const view = (code: string, games: number, extra: Partial<RulesetView["ruleset"]> = {}): RulesetView => ({
  code,
  ruleset: { ...defaultRuleset(), name: "NorCal Weekly", version: 3, by: "sarah", ...extra },
  hash: "0".repeat(64),
  createdAt: "2026-10-11T12:00:00.000Z",
  newer: null,
  link: `https://brawlonline.net/r/${code}`,
  games,
});

const log = (what: string) => () => console.log(what);

/** The list's rows: the code with its copy icon on the left, "played N times" on the summary line, "Use". */
export const Rows = () => (
  <div style={{ width: 520 }}>
    <PublishedRow
      view={view("KFQBTR", 1423, { bestOf: 3, strikes: [1, 2, 1], bans: 2 })}
      inUse={false}
      selected={false}
      onView={log("view")}
      onEdit={log("edit")}
      onCopy={(t) => console.log("copied", t)}
      onExport={log("export")}
      onToggleHidden={log("hide")}
      onUse={log("use")}
    />
    <PublicRow
      view={view("QBHFSM", 1, { name: "Random Fun", stageChoice: "random", version: 1 })}
      inUse={false}
      selected={true}
      onView={log("view")}
      onCopy={(t) => console.log("copied", t)}
      onUse={log("use")}
    />
    <PublicRow
      view={view("ZXCVBN", 0, { name: "Brand new", version: 1 })}
      inUse={true}
      selected={false}
      onView={log("view")}
      onCopy={(t) => console.log("copied", t)}
      onUse={log("use")}
    />
  </div>
);

/** The opened ruleset's header: the same code chip. */
export const Header = () => (
  <div className={styles.editorHeader} style={{ width: 520 }}>
    <Typography variant="h6">NorCal Weekly v3</Typography>
    <CodeCopy code="KFQBTR" onCopy={(t) => console.log("copied", t)} className={styles.headerCode} />
  </div>
);
