# 受付経路の同一端末接続設定

Vo の本体起動確認で、Ex が提供する非 loopback HTTP の Cr 接続先が
アプリの安全検証に拒否された。Di の service-token client と受付 client も
HTTPS または loopback HTTP のみを許可しているため、同一端末の catalog env に
Cr と Vo の loopback 接続先を明示する。検証を緩めず、固定トークンへ迂回しない。

ポートは Cernere/excubitor.catalog.yaml の cernere:8080、
Voluptas/excubitor.catalog.yaml の volputas:8892 と照合した。
将来別端末へ移す場合は、HTTPS 接続先へこの配置設定を更新する。

受付 API のコードは #2542 でマージ済み。この変更の反映には Ex による再起動が必要。
