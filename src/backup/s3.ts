/**
 * S3 への単一ファイル PutObject (アーカイブ push).
 *
 * - StorageClass を指定して Glacier 系へ安価にアーカイブできる。
 * - endpoint 指定で MinIO 等の S3 互換ストレージにも流せる (forcePathStyle)。
 * - 認証情報未指定なら AWS SDK の既定チェーン (環境変数 / IAM ロール) に委ねる。
 */

import { S3Client, PutObjectCommand, type StorageClass } from "@aws-sdk/client-s3";
import { createReadStream, statSync } from "node:fs";

export interface S3UploadArgs {
  bucket: string;
  key: string;
  filePath: string;
  region: string;
  storageClass?: string;
  endpoint?: string;
  accessKeyId?: string;
  secretAccessKey?: string;
}

export interface S3UploadResult {
  bucket: string;
  key: string;
  bytes: number;
  storageClass: string;
}

export async function uploadFileToS3(args: S3UploadArgs): Promise<S3UploadResult> {
  const client = new S3Client({
    region: args.region,
    ...(args.endpoint ? { endpoint: args.endpoint, forcePathStyle: true } : {}),
    ...(args.accessKeyId && args.secretAccessKey
      ? { credentials: { accessKeyId: args.accessKeyId, secretAccessKey: args.secretAccessKey } }
      : {}),
  });

  const bytes = statSync(args.filePath).size;
  const storageClass = args.storageClass ?? "STANDARD";

  try {
    await client.send(
      new PutObjectCommand({
        Bucket: args.bucket,
        Key: args.key,
        Body: createReadStream(args.filePath),
        // stream を Body にする場合 S3 は長さを自分で測れないため明示する。
        ContentLength: bytes,
        ContentType: "application/gzip",
        StorageClass: storageClass as StorageClass,
      })
    );
  } finally {
    client.destroy();
  }

  return { bucket: args.bucket, key: args.key, bytes, storageClass };
}
