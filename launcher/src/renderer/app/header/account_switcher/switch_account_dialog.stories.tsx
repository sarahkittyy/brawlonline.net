import type { StoredAccount } from "@settings/types";
import type { Meta, StoryObj } from "@storybook/react-webpack5";
import { action } from "storybook/actions";

import { generateDisplayPicture } from "@/lib/display_picture";

import { SwitchAccountDialog } from "./switch_account_dialog";

const account = (id: string, displayName: string, day: number): StoredAccount => ({
  id,
  email: `${displayName.toLowerCase()}@example.com`,
  displayName,
  displayPicture: generateDisplayPicture(id),
  lastActive: new Date(`2025-12-0${day}T10:00:00Z`),
});

const accounts = [
  account("abc123", "sarahkitty", 9),
  account("def456", "FalcoMain", 8),
  account("ghi789", "FoxMaster", 7),
  account("jkl012", "MarthPro", 6),
  account("mno345", "SheikPlayer", 5),
];

const meta = {
  title: "containers/Header/SwitchAccountDialog",
  component: SwitchAccountDialog,
  args: {
    open: true,
    onClose: action("onClose"),
    accounts: accounts.slice(0, 1),
    activeAccountId: "abc123",
    onSwitchAccount: action("onSwitchAccount"),
    onAddAccount: action("onAddAccount"),
    onRemoveAccount: action("onRemoveAccount"),
    switching: false,
  },
} satisfies Meta<typeof SwitchAccountDialog>;

export default meta;

type Story = StoryObj<typeof meta>;

// Only the account logged in: it and "Add another account"
export const OneAccount: Story = {};

export const ThreeAccounts: Story = { args: { accounts: accounts.slice(0, 3) } };

// Five, the most: no "Add another account"
export const FiveAccounts: Story = { args: { accounts } };

export const Switching: Story = { args: { accounts: accounts.slice(0, 3), switching: true } };
