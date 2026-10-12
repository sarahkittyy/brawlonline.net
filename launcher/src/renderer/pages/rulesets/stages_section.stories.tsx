import type { StageLists } from "@common/rulesets";
import { defaultRuleset } from "@common/rulesets";
import { useState } from "react";

import type { StageRole } from "./stages_section";
import { StagesSection } from "./stages_section";

export default {
  title: "pages/Rulesets/StagesSection",
  argTypes: {},
};

/** No stage art: Storybook has no extracted P+ files, as before the launcher extracts them. */
const noIcon = () => undefined;

const Editable = ({ initial, width = 560 }: { initial: StageLists; width?: number }) => {
  const [stages, setStages] = useState(initial);
  const onRole = (id: number, role: StageRole) => {
    const starters = stages.starters.filter((s) => s !== id);
    const counterpicks = stages.counterpicks.filter((s) => s !== id);
    if (role === "starter") {
      starters.push(id);
    } else if (role === "counterpick") {
      counterpicks.push(id);
    }
    setStages({ starters, counterpicks });
  };
  return (
    <div style={{ width }}>
      <StagesSection stages={stages} onRole={onRole} icon={noIcon} />
    </div>
  );
};

/** P+ default: the legal stages, the other stages off and collapsed. */
export const PPlusDefault = () => <Editable initial={defaultRuleset().stages} />;

/** A casual list with stages outside the legal ones: "Other stages" opens by itself. */
export const OtherStagesOn = () => (
  <Editable
    initial={{
      starters: [0x01, 0x02, 0x21],
      counterpicks: [0x2a, 0x07, 0x4f],
    }}
  />
);

/** The editor column at the narrowest the Rulesets page makes it. */
export const NarrowColumn = () => <Editable initial={defaultRuleset().stages} width={380} />;
