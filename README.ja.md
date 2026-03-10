# serialized_topic_monitor

`rclcpp::SerializedMessage` を使って、メッセージを deserialize せずに ROS 2 トピックの
通信状態を監視し、Web/UI 向けの JSON を publish する C++ ノードです。

[English README](./README.md)

## 概要

`serialized_topic_monitor` は、実運用を意識した ROS 2 通信監視のためのバックエンドです。

- トピックごとの publish 周波数（Hz）を算出
- 帯域（bytes/s, MiB/s）を推定
- メッセージ到着間隔から stale 状態を判定
- UI/バックエンドが使いやすい JSON を定期 publish
- ノード/トピック/QoS 情報を含むグラフ JSON を生成

同一リポジトリ内の `topic_monitor_web_server` と組み合わせて使う想定です。

## 特徴

- `create_generic_subscription` による型文字列ベース購読
- メッセージ型ごとのテンプレート実装が不要
- 大容量トピック（Image/PointCloud など）でも payload を展開せず監視可能
- 実行時フィルタ:
  - `allowlist`
  - `denylist`
  - hidden topic 除外
  - ROS 内部トピック除外
- 監視周期・出力周期・stale 判定閾値をパラメータで調整可能
- スライディングウィンドウで Hz/帯域の値を安定化

## ノード

- 実行ファイル名: `serialized_topic_monitor_node`
- ノード名: `serialized_topic_monitor_node`

## Publish トピック

### 1) Stats JSON

- 既定トピック: `/topic_monitor/stats_json`
- 型: `std_msgs/msg/String`
- 内容: トピックごとの統計値

主なフィールド:

- `generated_at_sec`
- `topic_count`
- `topics[]`:
  - `name`
  - `type`
  - `publisher_count`
  - `subscriber_count`
  - `alive`
  - `stale`
  - `hz`
  - `bandwidth_bytes_per_sec`
  - `bandwidth_mib_per_sec`
  - `latest_message_size_bytes`
  - `latest_message_size_mib`
  - `message_count`
  - `age_sec`（未受信時は `null`）

### 2) Graph JSON

- 既定トピック: `/topic_monitor/graph_json`
- 型: `std_msgs/msg/String`
- 内容: トポロジ表示向けグラフ情報

主なフィールド:

- `node_count`
- `edge_count`
- `nodes[]`:
  - `id`, `label`
  - `node_type`（`host | namespace | node | topic`）
  - `status`（`ok | stale | down | neutral`）
  - `host`
  - `node_namespace`
  - `parent_id`
- `edges[]`:
  - `id`, `source`, `target`
  - `qos`（簡易表示）
  - 詳細 QoS:
    - `qos_reliability`
    - `qos_durability`
    - `qos_history`
    - `qos_depth`
    - `qos_liveliness`
    - `qos_deadline_sec`
    - `qos_lifespan_sec`
    - `qos_liveliness_lease_duration_sec`
    - `qos_avoid_ros_namespace_conventions`

## パラメータ

### フィルタ関連

- `allowlist`（`string[]`、既定: `[]`）
  - 空でなければ指定トピックのみ監視します。
- `denylist`（`string[]`、既定: `["/parameter_events", "/rosout"]`）
  - 指定トピックを監視対象から除外します。
- `include_hidden_topics`（`bool`、既定: `false`）
  - hidden topic を含めるか。
- `skip_internal_topics`（`bool`、既定: `true`）
  - ROS 内部トピックを除外するか。

### 周期・推定関連

- `scan_period_ms`（`int`、既定: `1000`）
  - ROS graph の再走査周期。
- `report_period_ms`（`int`、既定: `1000`）
  - JSON publish 周期。
- `stale_timeout_sec`（`double`、既定: `2.0`）
  - 最終受信からこの秒数を超えると stale 判定。
- `window_size`（`int`、既定: `20`、実効最小値: `2`）
  - Hz/帯域推定に使うスライディングウィンドウサイズ。

### 出力トピック関連

- `stats_topic`（`string`、既定: `/topic_monitor/stats_json`）
- `graph_topic`（`string`、既定: `/topic_monitor/graph_json`）

## ビルド

ワークスペースルートで実行:

```bash
colcon build --packages-select serialized_topic_monitor
source install/setup.bash
```

## 起動

```bash
ros2 run serialized_topic_monitor serialized_topic_monitor_node
```

代表的な起動例:

```bash
ros2 run serialized_topic_monitor serialized_topic_monitor_node --ros-args \
  -p allowlist:="['/points_raw','/camera/image_raw','/odom']" \
  -p denylist:="['/parameter_events','/rosout']" \
  -p include_hidden_topics:=false \
  -p skip_internal_topics:=true \
  -p report_period_ms:=1000 \
  -p scan_period_ms:=1000 \
  -p stale_timeout_sec:=2.0 \
  -p window_size:=20 \
  -p stats_topic:='/topic_monitor/stats_json' \
  -p graph_topic:='/topic_monitor/graph_json'
```

## テスト

```bash
colcon test --packages-select serialized_topic_monitor
colcon test-result --verbose
```

## 実装メモ

- 帯域はシリアライズ済みメッセージサイズから推定しています。
- 計算式:
  - `hz = (N - 1) / (t_last - t_first)`
  - `bandwidth_bytes_per_sec = sum(size[1..N-1]) / (t_last - t_first)`
- 依存を増やさないため、JSON は手動でエスケープして組み立てています。

## 制約

- graph の host 分類は現在ローカルホスト名ベースです。
- QoS は graph API から得られる情報に依存し、複雑なネットワークでは見え方に差が出る場合があります。
- 本パッケージは通信状態監視が目的で、メッセージ内容の意味的妥当性検証は行いません。
