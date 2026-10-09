export const PagedListStatusMessages = {
  rateLimited: (seconds: number) => `Too many requests. Retrying in ${seconds} s.`,
  failed: (message: string) => `Could not load more: ${message}`,
  retry: () => "Retry",
};
