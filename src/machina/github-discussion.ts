/**
 * GitHub Discussions への要約投稿。
 *
 * GraphQL API を使用。必要な環境変数:
 *   - GITHUB_TOKEN : `discussion:write` スコープ付きの PAT
 *
 * 呼び出し側は channel_monitors.github_repo (`owner/repo`) と
 * 任意で categoryId を設定する。
 *
 * GITHUB_TOKEN が未設定なら、何もせず null を返す (フォールバック)。
 */

const GITHUB_API = "https://api.github.com/graphql";

export interface PublishArgs {
  /** owner/repo 形式 */
  repo: string;
  /** 未指定なら最初の "General" 相当カテゴリを自動探索 */
  categoryId: string | null;
  title: string;
  body: string;
}

export interface PublishResult {
  id: string;
  url: string;
}

export async function publishToGithubDiscussion(
  args: PublishArgs
): Promise<PublishResult | null> {
  const token = process.env.GITHUB_TOKEN;
  if (!token) {
    console.log("[github-discussion] GITHUB_TOKEN 未設定のためスキップ");
    return null;
  }

  const [owner, name] = args.repo.split("/");
  if (!owner || !name) {
    throw new Error(`リポジトリは owner/repo 形式で指定してください: ${args.repo}`);
  }

  // repositoryId とデフォルトカテゴリを取得
  const meta = await fetchRepoMeta(token, owner, name);
  const categoryId = args.categoryId || pickDefaultCategory(meta.categories);
  if (!categoryId) {
    throw new Error("Discussion category が見つかりませんでした。GitHub でカテゴリを作成してください。");
  }

  const mutation = `
    mutation($input: CreateDiscussionInput!) {
      createDiscussion(input: $input) {
        discussion { id url }
      }
    }
  `;
  const variables = {
    input: {
      repositoryId: meta.id,
      categoryId,
      title: args.title,
      body: args.body,
    },
  };

  const res = await fetch(GITHUB_API, {
    method: "POST",
    headers: {
      Authorization: `bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ query: mutation, variables }),
  });
  if (!res.ok) throw new Error(`GitHub GraphQL HTTP ${res.status}`);
  const data = (await res.json()) as {
    data?: { createDiscussion?: { discussion: { id: string; url: string } } };
    errors?: Array<{ message: string }>;
  };
  if (data.errors && data.errors.length > 0) {
    throw new Error(`GitHub GraphQL error: ${data.errors.map((e) => e.message).join(", ")}`);
  }
  const disc = data.data?.createDiscussion?.discussion;
  if (!disc) throw new Error("GitHub GraphQL: Discussion が作成されませんでした");
  return { id: disc.id, url: disc.url };
}

async function fetchRepoMeta(
  token: string,
  owner: string,
  name: string
): Promise<{
  id: string;
  categories: Array<{ id: string; name: string }>;
}> {
  const query = `
    query($owner: String!, $name: String!) {
      repository(owner: $owner, name: $name) {
        id
        discussionCategories(first: 25) {
          nodes { id name }
        }
      }
    }
  `;
  const res = await fetch(GITHUB_API, {
    method: "POST",
    headers: {
      Authorization: `bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ query, variables: { owner, name } }),
  });
  if (!res.ok) throw new Error(`GitHub GraphQL HTTP ${res.status}`);
  const data = (await res.json()) as {
    data?: {
      repository?: {
        id: string;
        discussionCategories: { nodes: Array<{ id: string; name: string }> };
      };
    };
    errors?: Array<{ message: string }>;
  };
  if (data.errors && data.errors.length > 0) {
    throw new Error(`GitHub GraphQL error: ${data.errors.map((e) => e.message).join(", ")}`);
  }
  const repo = data.data?.repository;
  if (!repo) throw new Error(`リポジトリ ${owner}/${name} が見つかりませんでした`);
  return { id: repo.id, categories: repo.discussionCategories.nodes };
}

function pickDefaultCategory(
  categories: Array<{ id: string; name: string }>
): string | null {
  if (categories.length === 0) return null;
  const general = categories.find((c) => /general|general|雑談|ディスカッション/i.test(c.name));
  return general?.id ?? categories[0].id;
}
