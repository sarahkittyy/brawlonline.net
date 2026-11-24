export const TwitchMessages = {
  twitch: () => "Twitch",
  connectAccount: () => "Connect account",
  disconnect: () => "Disconnect",
  /** A live mark's hover text and label, as the profile's social links have theirs. */
  account: (login: string) => `Twitch: @${login}`,
};
