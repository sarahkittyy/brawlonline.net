import { PRODUCT_NAME } from "@common/product";
import Typography from "@mui/material/Typography";

import { OnlineRulesMessages as Messages } from "./online_rules.messages";
import styles from "./online_rules.module.css";

export const OnlineRules = () => {
  return (
    <div>
      <Typography className={styles.sectionHeader}>{Messages.onlineRules(PRODUCT_NAME)}</Typography>
      <div className={styles.rulesContainer}>
        <Typography>{Messages.onlineRulesDescription(PRODUCT_NAME)}</Typography>
        <div className={styles.rulesList}>
          <Typography>1.</Typography>
          <Typography>{Messages.rule1()}</Typography>
          <Typography>2.</Typography>
          <Typography>{Messages.rule2(PRODUCT_NAME)}</Typography>
          <Typography>3.</Typography>
          <Typography>{Messages.rule3()}</Typography>
          <Typography>4.</Typography>
          <Typography>{Messages.rule4()}</Typography>
        </div>
      </div>
    </div>
  );
};
