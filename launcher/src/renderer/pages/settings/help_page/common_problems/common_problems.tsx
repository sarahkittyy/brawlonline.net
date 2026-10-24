import { ExternalLink as A } from "@/components/external_link";

import styles from "../support_box/support_box.module.css";
import { CommonProblemsMessages as Messages } from "./common_problems.messages";

// Microsoft's permanent link to the latest Visual C++ Redistributable (2015 and newer, x64).
const VC_REDIST_URL = "https://aka.ms/vc14/vc_redist.x64.exe";

// The problems an error message sends the player here for (handle_dolphin_exit_code.ts).
export const CommonProblems = () => {
  return (
    <div className={styles.container}>
      <h2 className={styles.iconContainer}>{Messages.commonProblems()}</h2>
      <h3>{Messages.dllsMissing()}</h3>
      <p>
        {Messages.installRedist()}{" "}
        <A className={styles.link} href={VC_REDIST_URL}>
          vc_redist.x64.exe
        </A>
      </p>
      <p>{Messages.windowsN()}</p>
      <p>{Messages.antivirus()}</p>
    </div>
  );
};
