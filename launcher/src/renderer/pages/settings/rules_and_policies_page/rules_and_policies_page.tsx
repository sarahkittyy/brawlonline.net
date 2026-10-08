import React from "react";

import { OnlineRules } from "@/components/online_rules/online_rules";

import { RulesAndPoliciesPageMessages as Messages } from "./rules_and_policies_page.messages";

export const RulesAndPoliciesPage = React.memo(() => {
  return (
    <div>
      <h1>{Messages.rulesAndPolicies()}</h1>
      <OnlineRules />
    </div>
  );
});
