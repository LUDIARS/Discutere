# Thaleia 向け persona consensus / gap API

## 目的

Thaleia が Discutere のペルソナ別議論集計を読み、施策を design gap として起票できるようにする。

## 完了条件

- `GET /api/admin/consensus/:gapId/personas` が traits、発話、各スコア、同意率を返す。
- `POST /api/admin/gaps` が既存の headless discussion seed を通じて gap と session を作る。
- 純粋集計のテストと API 契約文書を追加する。
