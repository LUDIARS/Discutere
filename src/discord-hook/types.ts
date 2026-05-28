export interface DiscordInboundAuthor {
  id: string;
  username: string;
  bot?: boolean;
}

export interface DiscordInboundMention {
  id?: string;
  username?: string;
}

export interface DiscordInboundMessage {
  id: string;
  channelId: string;
  author: DiscordInboundAuthor;
  content: string;
  mentions: DiscordInboundMention[];
  timestamp: string;
}

