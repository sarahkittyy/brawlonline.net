import Tooltip from "@mui/material/Tooltip";
import log from "electron-log";
import type { NetplayNetwork } from "main/netplay_networks";
import React from "react";

import { Dropdown } from "@/components/form/dropdown";
import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useNetplayNetwork } from "@/lib/hooks/use_settings";

import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";

/**
 * "Network for online games": Automatic, or one of the PC's addresses (a VPN adapter's, such as
 * ZeroTier's), which friends in Direct and rooms who picked the same network reach this PC by
 * (Dolphin's Config::GetChosenNetwork).
 */
export const NetplayNetworkSetting = () => {
  const [netplayNetwork, setNetplayNetwork] = useNetplayNetwork();
  const netplayDolphinOpen = useDolphinStore((store) => store.netplayOpened);
  const [networks, setNetworks] = React.useState<NetplayNetwork[]>([]);

  React.useEffect(() => {
    // Read again when Dolphin closes: a VPN may have come up in the meantime.
    void window.electron.common.listNetplayNetworks().then(setNetworks).catch(log.error);
  }, [netplayDolphinOpen]);

  const options = React.useMemo(() => {
    const list = [{ value: "", label: Messages.netplayNetworkAuto() }];
    for (const { name, cidr } of networks) {
      list.push({ value: cidr, label: `${name} (${cidr.split("/")[0]})` });
    }
    // The chosen adapter is down (or got another address): keep it, so it is not silently changed.
    if (netplayNetwork && !networks.some(({ cidr }) => cidr === netplayNetwork)) {
      list.push({ value: netplayNetwork, label: Messages.netplayNetworkMissing(netplayNetwork.split("/")[0]) });
    }
    return list;
  }, [networks, netplayNetwork]);

  return (
    <SettingItem name={Messages.netplayNetwork()} description={Messages.netplayNetworkDescription()}>
      <Tooltip title={netplayDolphinOpen ? Messages.closeDolphinToChange() : ""} placement="bottom-start">
        <div>
          <Dropdown
            value={netplayNetwork}
            options={options}
            onChange={(value) => void setNetplayNetwork(value)}
            disabled={netplayDolphinOpen}
          />
        </div>
      </Tooltip>
    </SettingItem>
  );
};
