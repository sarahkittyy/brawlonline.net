import { PRODUCT_NAME } from "@common/product";
import { css } from "@emotion/react";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import CircularProgress from "@mui/material/CircularProgress";
import FormControlLabel from "@mui/material/FormControlLabel";
import { useState } from "react";

import { OnlineRules } from "@/components/online_rules/online_rules";
import { UsagePolicyList } from "@/components/usage_policy_list/usage_policy_list";
import { refreshUserData } from "@/lib/hooks/use_account";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useServices } from "@/services";

import { StepContainer } from "../../step_container";
import { AcceptRulesStepMessages as Messages } from "./accept_rules_step.messages";

export const AcceptRulesStep = () => {
  const { backendService } = useServices();
  const { showError } = useToasts();
  const [rulesChecked, setRulesChecked] = useState(false);
  const [policiesChecked, setPoliciesChecked] = useState(false);
  const [processing, setProcessing] = useState(false);

  const handleAcceptClick = async () => {
    setProcessing(true);

    try {
      await backendService.acceptRules();
      await refreshUserData(backendService);
    } catch (err: any) {
      showError(err.message);
    } finally {
      setProcessing(false);
    }
  };

  return (
    <StepContainer header={Messages.acceptRulesAndPolicies()}>
      <OnlineRules />
      <FormControlLabel
        label={Messages.acceptOnlineRules(PRODUCT_NAME)}
        control={
          <Checkbox
            checked={rulesChecked}
            disabled={processing}
            onChange={(_event, value) => setRulesChecked(value)}
            sx={{ "& .MuiSvgIcon-root": { fontSize: 28 } }}
          />
        }
      />
      <UsagePolicyList />
      <FormControlLabel
        label={Messages.acceptPrivacyPolicyAndTos(PRODUCT_NAME)}
        control={
          <Checkbox
            checked={policiesChecked}
            disabled={processing}
            onChange={(_event, value) => setPoliciesChecked(value)}
            sx={{ "& .MuiSvgIcon-root": { fontSize: 28 } }}
          />
        }
      />
      <div>
        <Button
          css={css`
            margin-top: 32px;
            width: 150px;
            height: 54px;
          `}
          onClick={handleAcceptClick}
          variant="contained"
          disabled={!policiesChecked || !rulesChecked || processing}
          size="large"
        >
          {processing ? <CircularProgress color="inherit" size={24} /> : Messages.acceptAll()}
        </Button>
      </div>
    </StepContainer>
  );
};
