import { css } from "@emotion/react";
import styled from "@emotion/styled";
import AddIcon from "@mui/icons-material/Add";
import CheckIcon from "@mui/icons-material/Check";
import CloseIcon from "@mui/icons-material/Close";
import ButtonBase from "@mui/material/ButtonBase";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import type { StoredAccount } from "@settings/types";
import React from "react";

import { MainPicture } from "@/components/main_picture/main_picture";

import { AccountSwitcherMessages as Messages } from "./account_switcher.messages";

const MAX_ADDITIONAL_ACCOUNTS = 4;

const AccountItem = styled.div<{ $disabled?: boolean; $current?: boolean }>`
  display: flex;
  justify-content: space-between;
  align-items: center;
  min-height: 42px;
  padding: 4px 16px;
  border-radius: 4px;
  cursor: ${(props) => (props.$disabled || props.$current ? "default" : "pointer")};
  opacity: ${(props) => (props.$disabled ? 0.6 : 1)};
  transition: background-color 100ms ease-out;

  &:hover {
    background-color: ${(props) => (props.$disabled || props.$current ? "transparent" : "rgba(255, 255, 255, 0.05)")};

    .remove-button {
      opacity: ${(props) => (props.$disabled ? 0 : 1)};
    }
  }
`;

const AccountInfo = styled.div`
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  flex: 1;
  min-width: 0;
`;

const AccountName = styled.div`
  font-size: 16px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  width: 100%;
`;

const SectionDivider = styled.div`
  height: 1px;
  background-color: rgba(255, 255, 255, 0.1);
  margin: 8px 0;
`;

const ActionButton = styled(ButtonBase)`
  width: 100%;
  display: flex;
  align-items: center;
  padding: 10px 12px;
  gap: 8px;
  justify-content: center;
  border-radius: 4px;
  font-size: 14px;
  transition: background-color 100ms ease-out;

  &:hover {
    background-color: rgba(255, 255, 255, 0.05);
  }
`;

const RemoveButton = styled(IconButton)`
  padding: 4px;
  color: var(--text-dim);
  opacity: 0;

  &:hover {
    background-color: rgba(255, 255, 255, 0.05);
  }
`;

type AccountSwitcherProps = {
  accounts: readonly StoredAccount[]; // Every account, the active one included
  activeAccountId: string | null;
  onSwitchAccount: (accountId: string) => void;
  onAddAccount: () => void;
  onRemoveAccount: (accountId: string) => void;
  switching?: boolean;
  /** Each account's picture by its id (MainPicture's `url`); missing: still loading. */
  pictures?: Partial<Record<string, string | null>>;
};

/**
 * Switch account's list: the account logged in first (a check, not clickable), then the others by when they were last
 * used (click one to switch, the X removes it), then "Add another account" while there's room.
 */
export const AccountSwitcher = ({
  accounts,
  activeAccountId,
  onSwitchAccount,
  onAddAccount,
  onRemoveAccount,
  switching = false,
  pictures = {},
}: AccountSwitcherProps) => {
  const activeAccount = accounts.find((account) => account.id === activeAccountId);
  // The others, by last active
  const sortedAccounts = React.useMemo(() => {
    return accounts
      .filter((account) => account.id !== activeAccountId)
      .sort((a, b) => b.lastActive.getTime() - a.lastActive.getTime())
      .slice(0, MAX_ADDITIONAL_ACCOUNTS);
  }, [accounts, activeAccountId]);

  const handleAccountClick = (accountId: string) => {
    if (!switching) {
      onSwitchAccount(accountId);
    }
  };

  const handleRemoveClick = (e: React.MouseEvent, accountId: string) => {
    e.stopPropagation(); // Prevent triggering the account switch
    onRemoveAccount(accountId);
  };

  return (
    <div
      css={css`
        color: var(--off-white);
      `}
    >
      {activeAccount && (
        <AccountItem $current={true} data-testid="active-account">
          <AccountRow account={activeAccount} picture={pictures[activeAccount.id]} />
          <CheckIcon sx={{ fontSize: 20, color: "var(--accent-primary)", marginRight: "4px" }} />
        </AccountItem>
      )}

      {/* The other accounts */}
      {sortedAccounts.length > 0 && (
        <div
          css={css`
            display: flex;
            flex-direction: column;
            gap: 2px;
          `}
        >
          {sortedAccounts.map((account) => (
            <AccountItem key={account.id} onClick={() => handleAccountClick(account.id)} $disabled={switching}>
              <AccountRow account={account} picture={pictures[account.id]} />
              <Tooltip title={Messages.remove()}>
                <RemoveButton className="remove-button" onClick={(e) => handleRemoveClick(e, account.id)} size="small">
                  <CloseIcon sx={{ fontSize: 18 }} />
                </RemoveButton>
              </Tooltip>
            </AccountItem>
          ))}
        </div>
      )}

      {/* Add Account Button */}
      {sortedAccounts.length < MAX_ADDITIONAL_ACCOUNTS && (
        <>
          <SectionDivider />
          <ActionButton onClick={onAddAccount}>
            <AddIcon sx={{ fontSize: 18 }} />
            {Messages.addAnotherAccount()}
          </ActionButton>
        </>
      )}
    </div>
  );
};

const AccountRow = ({ account, picture }: { account: StoredAccount; picture: string | null | undefined }) => (
  <div
    css={css`
      display: flex;
      align-items: center;
      gap: 10px;
      min-width: 0;
    `}
  >
    <MainPicture url={picture} size={26} />
    <AccountInfo>
      <AccountName>{account.displayName || account.email}</AccountName>
    </AccountInfo>
  </div>
);
