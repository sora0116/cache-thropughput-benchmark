# Cache Throughput Benchmark

このリポジトリは、キャッシュ階層ごとの `read` / `write` スループットを測るための小さなベンチマークです。

現時点では、混合ワークロード全体を 1 回の `perf stat` で推定するのではなく、各階層を個別の PMU プローブで測ってから share で合成する方針を取っています。これにより、`100/0/0/0` のような高い `L1` 指定でも、イベント多重化や混合ワークロード由来のノイズを減らしています。

## Build

```sh
make
```

## Run

```sh
./run_bench.py --build --mode both --target-l1 60 --target-l2 20 --target-l3 10 --target-dram 10
```

CSV は既定で `results.csv` に保存されます。

比率マトリクスを一括評価したい場合は `evaluate_matrix.py` を使います。

```sh
./evaluate_matrix.py --build --mode read --step 20 --output matrix_results.csv
```

`--step 20` なら `0,20,40,60,80,100` の格子点から `l1+l2+l3+dram=100` を満たす全組み合わせを実行します。

CSV から `actual_*` をそのまま使って可視化したい場合は `plot_matrix_slice.py` を使います。

```sh
./plot_matrix_slice.py \
  --input matrix_results.csv \
  --mode read \
  --x-axis l1 \
  --color-axis l2 \
  --value ops_per_cycle \
  --output actual_scatter.png
```

この例では、全測定点をそのまま使い、`actual L1` を横軸、`ops_per_cycle` を縦軸、`actual L2` を色で表した散布図を出します。固定軸の近傍抽出や許容幅による絞り込みは行いません。

`actual_l1` から `actual_dram` までをそれぞれ x 軸にして、throughput との関係をまとめて見たい場合は `plot_actual_throughput.py` を使います。

```sh
./plot_actual_throughput.py \
  --input matrix_results.csv \
  --mode read \
  --value gbps \
  --output actual_throughput.png
```

この例では、`actual_l1` / `actual_l2` / `actual_l3` / `actual_dram` を x 軸、`gbps` を y 軸にした 4 枚の散布図を 1 枚にまとめて出力します。

命令数を見たい場合は、同じスクリプトで `instructions_per_op` や `pmu_instructions` も描けます。

```sh
./plot_actual_throughput.py \
  --input matrix_results.csv \
  --mode read \
  --value instructions_per_op \
  --output instructions_per_op.png
```

## 方針

### 1. ヒット率の扱い

- `actual_l1` / `actual_l2` / `actual_l3` / `actual_dram` は、現在は PMU 実測値です。
- ただし「混合ワークロードを 1 回だけ測る」のではなく、各階層を 100% で動かす専用プローブを個別に測り、その結果を share で合成しています。
- たとえば `80/10/5/5` なら、`L1 100%`, `L2 100%`, `L3 100%`, `DRAM 100%` のプローブ結果を取り、それを `0.80/0.10/0.05/0.05` で重み付けして最終の `actual_*` を出します。

この方針にした理由は次の通りです。

- `perf stat` で retired load source をまとめて読むと、多重化やベンチ外ロードの混入で値が不安定になりやすいです。
- 特に高い `L1` 比率では、単独の `L1` プローブを取る方が、混合ワークロードを 1 回測るより安定します。
- throughput 測定では、ヒット率推定のための pointer-chase や scrub が帯域を潰しやすいため、測定用カーネルを分離しています。

### 2. 校正と throughput 測定の分離

このベンチマークでは、目的の異なる 2 種類のカーネルを使います。

- `chain` kernel
  - 階層ごとに分離したワークロードを作るためのカーネルです。
  - 各キャッシュラインを pointer-chase で辿ります。
  - prefetch の効きを弱めたいときや、階層を強く分けたいときに向いています。
  - 高オーバーヘッドなので、最終 throughput 測定の主役にはしません。
- `stream` kernel
  - 最終的な `ops/cycle` と `GB/s` を測るためのカーネルです。
  - pointer dependency を外し、軽いストリームアクセスにしています。
  - 高い `L1` 指定で throughput が不自然に低く出るのを避けるため、最終測定はこちらを使います。

加えて `baseline` mode も残しています。

- `baseline` mode
  - `chain` / `stream` と同じループ骨格を持ちながら、実データアクセスを行わない比較用モードです。
  - PMU プローブ時に、ループ骨格だけが生む miss を差し引くために使います。

## 各ワークロードの詳細

### 1. L1 ワークロード

- 目的
  - できるだけ `L1` のみで回るアクセス列を作ること。
- 作り方
  - 小さいワーキングセットを使います。
  - 現在は `share_l1 > 0` なら常に `lines_l1 = 64` です。
- 補足
  - 高い `L1` では scrub を無効にします。
  - `100/0/0/0` のようなケースでは、`L1` 専用ワークロードとして扱います。

### 2. L2 ワークロード

- 目的
  - `L1` より大きく、`L2` に収めやすいワーキングセットを使うこと。
- 作り方
  - `share_l2 > 0` のときだけ有効にします。
  - 現在の既定値は `lines_l2 = 2048` です。
  - `share_l2 == 0` なら `lines_l2 = 64` に落として、不要な帯域消費を避けます。
- 補足
  - `L2` を目立たせたいケースでは `evict_passes = (1, 0, 0)` を使います。
  - これは `L1` だけを軽く崩し、`L2` の存在感を残す狙いです。

### 3. L3 ワークロード

- 目的
  - `L2` よりさらに大きい領域を使って、`L3` を含むアクセス列を作ること。
- 作り方
  - `share_l3 > 0` のときだけ有効にします。
  - 既定は `lines_l3 = 65536` です。
  - `share_l3 == 0` なら `lines_l3 = 64` に落とします。
- 補足
  - `L3` 比率が大きいときは `evict_passes = (1, 1, 0)` を使います。
  - これは `L1` と `L2` を軽く崩して、`L3` 側に寄せる狙いです。

### 4. DRAM ワークロード

- 目的
  - LLC を超えるような大きめのワーキングセットを使い、DRAM 寄りのアクセス列を作ること。
- 作り方
  - `share_dram > 0` のときだけ有効にします。
  - 既定は `lines_dram = 393216` です。
  - `share_dram == 0` なら `lines_dram = 64` に落とします。
- 補足
  - DRAM を明確に混ぜたいケースでは `evict_passes = (1, 1, 1)` を使います。

## 生成方法の詳細

### share

- `--target-l1` などで与えた比率は、内部では合計 100 の整数 share に丸めます。
- たとえば `80/10/5/5` は `share_l1=80, share_l2=10, share_l3=5, share_dram=5` になります。
- 現在の `actual_*` は、この share を PMU プローブ結果に掛けて合成した値です。

### lines

- 各階層ごとに独立した配列を持ちます。
- 各配列のサイズは `lines_*` で決まり、1 line は 64 byte です。
- 現在は throughput 比較の公平性を優先し、各階層で有効時の `lines_*` を固定しています。

### step

- 各配列の巡回順は `choose_steps()` で作ります。
- `chain` kernel ではこの step を使って pointer cycle を作ります。
- 現在はランダム化された巡回順で、単純な逐次 stride より prefetch に乗りにくくしています。

### scrub / eviction

- `evict_passes_l1/l2/l3` は、ある階層のアクセス前に上位キャッシュを軽く崩すためのパラメータです。
- 現在は次の 4 パターンを主に使います。
  - `(0, 0, 0)` 高い `L1` 用
  - `(1, 0, 0)` `L2` を出したい用
  - `(1, 1, 0)` `L3` を出したい用
  - `(1, 1, 1)` DRAM を強めたい用
- throughput の最終測定では scrub を無効にします。

## 現在の制約

- 出力されるヒット率は PMU 実測ですが、「混合ワークロードそのものの 1 発測定」ではなく、「単独プローブの重み付き合成」です。
- 中間的な混合比率、特に `L2/L3/DRAM` の細かい分離については、今後さらに改善の余地があります。
- `write` の階層比率表示は、同じ構成に対する `read` 側の PMU プローブ結果を再利用しています。
- `evaluate_matrix.py` は現在、各 target ごとに `run_bench.py` を逐次実行するだけの実装です。評価点数が多いと時間がかかります。

## 典型例

### `100/0/0/0`

- `L1` 専用ワークロードとして扱います。
- `lines_l1` は最小、他階層は事実上無効です。
- 最終 throughput は `stream` kernel で測ります。

### `80/10/5/5`

- 高い `L1` を維持しつつ、少量の `L2/L3/DRAM` を混ぜるケースです。
- `L1` は小さいワーキングセット、他階層は必要なぶんだけ有効にします。
- throughput は scrub なしの `stream` で測ります。

### `20/40/20/20`

- `L1` より `L2` を目立たせたいケースです。
- `L1` を軽く崩す scrub を入れつつ、`L2/L3/DRAM` の配列を有効にします。
- ただし現在の実装では、この種の中間混合は高 `L1` ケースより精度が低いです。

## 推奨環境

- Linux `x86_64`
- Intel 系 CPU
- 単一コア pinning が使えること

現状のメインフローは PMU 必須ではありませんが、将来 PMU ベースの比較や再校正を戻すことを想定して `perf` が使える環境だと検証しやすいです。
