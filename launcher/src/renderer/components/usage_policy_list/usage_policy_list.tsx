import { defaultServiceUrls, PRODUCT_NAME } from "@common/product";
import Typography from "@mui/material/Typography";

import { ExternalLink as A } from "@/components/external_link";

import { UsagePolicyListMessages as Messages } from "./usage_policy_list.messages";
import styles from "./usage_policy_list.module.css";

export const UsagePolicyList = () => {
  return (
    <div>
      <Typography className={styles.sectionHeader}>{Messages.privacyPolicyAndTermsOfService()}</Typography>
      <Typography color="var(--text-secondary)">{Messages.clickTheLinksBelow()}</Typography>
      <div className={styles.policiesList}>
        <Typography color="var(--text-secondary)">●</Typography>
        <Typography color="var(--text-secondary)">
          <A className={styles.link} href={`${defaultServiceUrls.website}/privacy`}>
            {Messages.privacyPolicy(PRODUCT_NAME)}
          </A>
        </Typography>
        <Typography color="var(--text-secondary)">●</Typography>
        <Typography color="var(--text-secondary)">
          <A className={styles.link} href={`${defaultServiceUrls.website}/terms`}>
            {Messages.termsOfService(PRODUCT_NAME)}
          </A>
        </Typography>
      </div>
    </div>
  );
};
