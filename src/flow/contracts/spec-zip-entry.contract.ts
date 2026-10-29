export default {
  post: (result: Buffer, _data: Buffer, _compression: number, maxBytes: number): true | string => {
    return result.length <= 64 * 1024 * 1024 && result.length <= maxBytes
      ? true : "ZIP output exceeds entry limit or remaining aggregate budget";
  },
  postThrow: (error: unknown, _data: Buffer, _compression: number, _maxBytes: number, label: string): true | string => {
    return error instanceof Error && error.message.startsWith(`${label}: `)
      ? true : "ZIP errors must identify the source";
  },
};
