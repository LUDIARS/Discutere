import {
  VECTOR_DIMS as CANONICAL_VECTOR_DIMS,
  VECTOR_SPEC_VERSION,
} from "@ludiars/sentiment-core";

// @spec sentiment 空間の一本化
export function assertCrawlerVectorSpec(localDimensions, localVersion) {
  if (localVersion !== VECTOR_SPEC_VERSION) {
    throw new Error(
      `sentiment vector version drift: crawler=${localVersion}, canonical=${VECTOR_SPEC_VERSION}`
    );
  }
  if (
    !Array.isArray(localDimensions)
    || localDimensions.length !== CANONICAL_VECTOR_DIMS.length
    || localDimensions.some((dimension, index) => dimension !== CANONICAL_VECTOR_DIMS[index])
  ) {
    throw new Error("sentiment vector dimension drift from @ludiars/sentiment-core");
  }
}
