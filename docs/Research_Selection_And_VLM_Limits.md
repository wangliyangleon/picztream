# 研究笔记：多样性子集选择与 VLM/小模型能力边界

- **日期**：2026-08-01
- **服务对象**：`docs/Intent_Curation_PRD.md` 决策十至十二（多样性预筛，ticket 04）与风险二、风险三
- **性质**：文献调研，只陈述已发表证据。**不含推荐方案，不含实现计划。**
- **取材约束**：优先 arXiv / CVF / ACM / 期刊原文、官方 model card 与一方 API 文档。只能找到二手来源的结论已显式标注为"未经一手核实"。

---

## 一、与决策直接相关的结论摘要

只列对本项目两个待定区域有判据意义的部分，展开见下文。

**关于多样性预筛（决策十至十二）**

1. 贪心 farthest-point 的 2-近似保证针对的是**一般度量空间的最坏情况**。本项目的距离信号是 `captured_at`，即一维实轴，而**一维下 max-min dispersion 有多项式时间的精确解**，已发表复杂度为 `O(n log n + kn)`（Ravi 等）与固定 k 时线性（Araki-Nakano）。也就是说，本项目所处的这个特例里，"接受 2 倍误差"这件事不是被复杂度逼出来的。见 §2.2。
2. max-min（离散点最小间距最大化）与 k-center（覆盖半径最小化）是两个不同的目标函数，共用同一个贪心。本项目代码里的实现是 max-min 形状。二者的近似保证与硬度结论要分开引用。见 §2.1。
3. **贪心 farthest-point 由构造优先吃两端点**。本项目 `core/curate/curate.cpp` 的 seed 是 `captured_at` 最大的那张，第二次挑选必然是离它时间最远的那张，即时间轴上最早的那张。两个时间端点在前两次挑选里被锁死，与内容质量无关。见 §2.3。
4. max-min 类目标对离群点的敏感性是目标函数的性质而非实现缺陷：目标值完全由极值决定。文献里对此的标准应对不是改贪心，而是**改目标**（允许丢弃 z 个点的 outlier 变体，Charikar 等 2001，3-近似），或换成**平均型目标**（facility location / max-sum），后者单个离群点只贡献 1/|V| 的权重而非支配全局。见 §2.3、§2.4。
5. 存在把"散开"与"质量/不选离群点"合成单一目标并带保证的框架：Borodin-Lee-Ye 的 max-sum diversification，目标为「单调 submodular 质量项 + λ·两两距离和」，贪心在基数约束下有 **2-近似**（arXiv:1203.6397 定理 1）。但它需要一个质量项，而本项目预筛阶段按决策十正好没有质量信息可用。见 §2.4。
6. DPP 提供了 quality × diversity 的乘性分解，但 MAP 推断 NP-hard，贪心只在 monotone kernel 的特例下有保证，一般非单调情形已知最好是 1/4-近似（Gillenwater 等 NeurIPS 2012）。见 §2.4。

**关于 VLM 与本地小模型（风险二、风险三）**

7. 「图 → 文字描述 → 下游纯文本生成」这个两段式有成体系的已发表工作（Socratic Models、PICa、LENS）。**中间描述的具体程度直接决定下游质量，这一点有直接可比的数字**：PICa 在 OK-VQA 上，只给 tags 是 44.6%，只给生成的 caption 是 46.9%，caption+tags 是 48.0%，给一条人写的 ground-truth caption 是 48.7%，给全部五条人写 caption 是 **53.3%**。同一个下游模型、同一个任务，差别全在中间描述的具体程度上。见 §3.1。
8. 同时有一个负面证据：在 MMMU 上给纯文本 LLM 补 OCR 或 LLaVA-1.5 生成的 caption，几乎没有增益（FLAN-T5-XXL 32.1% → +Caption 34.8%；Vicuna-13B 33.3% → +Caption 33.9%）。原文结论是「The application of OCR and captioning technologies does not yield a significant improvement」。**通用 caption 不携带下游需要的信息**，与第 7 点合起来说明：有用的不是"有描述"，而是"描述里恰好写了下游要用的东西"。见 §3.1。
9. **没有找到任何直接测量"从描述生成社交媒体文案"质量的已发表工作。** 上述证据全部来自 VQA / 推理类下游任务。这一条外推到本项目的文案场景是未验证的。
10. 关于 18 条目的列表选择与排序：**这个长度落在文献最常用的 window size 20 附近**，但已发表证据显示这是对大模型标定的，中等规模开源模型在同一形状上会出现结构性失败。Qin 等在 FLAN-UL2（20B）上跑 listwise 提示时，模型「will either just output few documents (e.g., '[1]'), an ordered list based on id ..., or text which is not parseable」，并把失败归为四类：Missing、Rejection、Repetition、Inconsistency。见 §3.2。
11. **位置偏置在这个任务形状上有量级级别的影响**：同一篇报告 gpt-3.5-turbo 在 TREC-DL2019 上，输入按 BM25 序时 NDCG@10 = 65.80，把输入顺序倒过来后降到 32.77。原因之一是失败时模型「fall back to the initial ranking」。见 §3.2。
12. **约束解码只保证语法，不保证语义。** JSONSchemaBench 原文写明「constrained decoding should not affect the generated output as it only filters out the invalid tokens. However, things become more complicated due to ambiguity of tokenization and distributional shifts」，并指出过度约束会「limit the expressive power of LMs, potentially preventing the generation of valid responses」。Ollama 官方文档也把"在提示里明说要 JSON"列为与 `format` 参数并行的必要做法。见 §3.3。
13. 约束解码对任务准确率的影响，**两个一手来源结论相反**：Tam 等测到 JSON-mode 下推理任务大幅退化（LLaMA-3-8B 在 Last Letter 上 70.07% → 28.00%），Geng 等（JSONSchemaBench）测到约束解码「consistently improves the performance of downstream tasks up to 4%」。分歧的可能来源见 §4。

---

## 二、Topic 1：仅用时间戳做多样性子集选择

### 2.1 两个被混用的目标函数

必须先区分，因为它们的近似保证与硬度结论不同，而同一个贪心同时被用于两者。

- **k-center**：选 k 个中心，最小化任一点到最近中心的最大距离。是**覆盖**目标。
- **max-min dispersion（也叫 p-dispersion / k-dispersion）**：选 k 个点，最大化被选点之间的最小两两距离。是**散开**目标。

`core/curate/curate.cpp` 的实现是后者的形状：对每个候选算它到已选集的最小时间差，选这个最小值最大的那个。

**贪心（farthest-first traversal / Gonzalez 规则）同时服务两者。** 近期的一篇 arXiv 论文明确写道，这个贪心「repeatedly chooses points, one by one from the given n, and adds them to an initially empty subset, so as to maximize the distance between the new point and those already selected」，它对 k-dispersion 是 1/2-近似，「this same greedy method also achieves a 2-approximation for the related k-center problem」（[arXiv:2511.00692](https://arxiv.org/html/2511.00692)）。

**max-min 的保证与硬度**（Ravi、Rosenkrantz、Tayi，*Operations Research* 42(2):299-310, 1994）：

- 距离**不**满足三角不等式时，除非 P=NP，**不存在**任何多项式时间的相对近似算法。
- 距离满足三角不等式时，该启发式「provides a performance guarantee of two」。
- 同一篇同时证明「obtaining a performance guarantee of less than two is NP-hard」，即 2 这个常数在一般度量下是紧的。
- MAX-AVG（最大化平均两两距离）版本的贪心保证是 **4**。

来源：[INFORMS Operations Research 原文页](https://pubsonline.informs.org/doi/10.1287/opre.42.2.299)。

对本项目的意义：`captured_at` 的差的绝对值是实轴上的度量，三角不等式成立，所以"无法近似"那条不适用，2-近似成立。但见下一节。

### 2.2 一维（时间戳）是特例，且是好特例

本项目按决策十二只沿 `captured_at` 做多样性，距离空间就是一维实轴。文献对这个维度有专门结论：

- **维度是硬度的分界线。** Wang 与 Kuo（*Information Processing Letters* 28:281-286, 1988）证明该问题「is NP-hard already for d=2」，而「admits a polynomial algorithm for d=1」（转述见 [arXiv:2511.00692](https://arxiv.org/html/2511.00692)）。也就是说一维是精确可解的那一侧。
- **一维精确解的已发表复杂度**：同一篇 arXiv 综述记载，Ravi、Rosenkrantz 与 Tayi 给出「an algorithm running in O(n log n + kn)」用于线上的这个问题；Araki 与 Nakano 后来改进到「O(2^k k^{2k} n) time, thereby providing a linear-time solution for fixed k」（[arXiv:2511.00692](https://arxiv.org/html/2511.00692)）。
- **MAX-AVG 的一维版本同样多项式可解**：Ravi 等原文给出「a polynomial-time algorithm for the 1-dimensional MAX-AVG dispersion problem」，并以此为基础得到二维 MAX-AVG 的 π/2 渐近保证（[INFORMS](https://pubsonline.informs.org/doi/10.1287/opre.42.2.299)）。
- **相邻问题的一维精确解也已知**：1D k-means / k-medians 在一维有精确多项式 DP，经典结果是 `O(kn²)` 时间、`O(kn)` 空间，后续工作进一步压缩（Grønlund 等，[arXiv:1701.07204](https://arxiv.org/abs/1701.07204)）。k-medoids / p-median 在受限几何结构下同样多项式可解（Dupin，[arXiv:1806.02098](https://ar5iv.labs.arxiv.org/html/1806.02098)）。
- Dupin 另一篇给出二维 Pareto front 上 max-min p-dispersion 的 `O(pn log n)` DP，并指出 Max-Sum-min 变体的 `O(pn³)` 复杂度「holds also in 1D, proving for the first time that Max-Sum-min p-dispersion is polynomial in 1D」（[arXiv:2002.11830](https://arxiv.org/abs/2002.11830)）。

**决策相关的含义**：本项目的规模是候选集 n 最多几百、预选集 k 约 18。上述任一精确算法在这个规模下都是微秒级。贪心的 2-近似是一般度量下不可改进的最优保证，但**在一维这个特例里它不是唯一选项**，也不是被复杂度逼出来的选择。文献没有说贪心在一维会退化得多差（下界构造通常在一般度量里给），但也没有任何文献声称贪心在一维就是最优的。

### 2.3 贪心 farthest-point 的具体失效模式

**(a) 目标函数本身由极值决定。** max-min 的取值等于被选集合里最近的一对的距离；k-center 的取值等于最远的那个未覆盖点的距离。两者都是极值统计量，因此单个异常点可以完全改写解。文献对此的处理方式是承认目标函数有问题并改目标（见 §2.4 的 outlier 变体），而不是修补贪心。一篇 fair-clustering with outliers 的论文对此的表述是「the presence of outliers can significantly alter the ... clustering objective」，并给出离群点夹在两簇之间时会把中心拖到次优位置的具体例子（[arXiv:2412.10923](https://arxiv.org/html/2412.10923v1)）。

**(b) 经验上偏好离群点与噪声点。** 点云采样文献里对 FPS（farthest point sampling，与本项目算法同构）的记载是直接的：「FPS tends to prioritize outliers, often leading to the selection of noisy points」，并在预备节补充「FPS often selects noisy points as center points in the denoising task」（MICAS，[arXiv:2411.16773](https://arxiv.org/html/2411.16773v2)，引言与 §3.1）。该领域为此提出了若干替代采样策略（如 [arXiv:2107.04291](https://arxiv.org/abs/2107.04291)、[arXiv:2509.13213](https://arxiv.org/abs/2509.13213)），这些工作的存在本身即是对 FPS 该缺陷的领域共识证据。

需要标注的限制：以上证据来自 3D 点云的高维几何场景，**没有找到专门针对一维时间轴上 FPS 离群点行为的实证论文**。把结论迁移到本项目属于合理外推，但不是被直接验证的。

**(c) 端点由构造被优先选中，这一条在本项目里是确定的而非概率的。** `core/curate/curate.cpp` 的 `greedy_pick` 在已选集为空时走兜底分支，取 `captured_at` 最大的那张作 seed，即时间上最新的那张；随后第一次 farthest-point 迭代必然选出离它时间差最大的那张，也就是时间上最早的那张。（笔记初稿把 seed 规则记成了 `by_captured_at_desc`，那是票 01 新增的**输出排序**比较器，不是 seed 规则，已订正；结论不变。）因此**前两次挑选被时间轴的两个端点占满**，与这两张照片的画质、内容毫无关系。若某次拍摄的首尾两张恰好是试机废片，这两个名额在预筛阶段就被消耗掉，且按决策十它们之后还要各消耗一次视觉评估调用。

一维下这一点尤其突出：在高维里"最远点"可能落在任意方向的边界上，分散在多处；在一维里边界只有两个点，退化成"必选最早那张和最晚那张"。

### 2.4 有没有原则性的方式在"散开"与"不选离群点"之间取舍

文献里有四类做法，都以**改目标函数**而非改搜索策略的方式实现该取舍。

**(1) Outlier 变体：显式允许丢弃 z 个点。** Charikar、Khuller、Mount、Narasimhan（SODA 2001）给出 k-center with outliers 的 3-近似；其形式是允许算法忽略 z 个输入点后再做聚类。「In 2001 Charikar et al. [SODA'01] presented a 3-approximation for the k-center problem with outliers」（[arXiv:1401.2874](https://arxiv.org/abs/1401.2874)）。k-median 的对应变体同样出自该文（[arXiv:2412.10923](https://arxiv.org/html/2412.10923v1)）。

- 这是与本项目场景最贴合的"原则性旋钮"：z 直接就是"允许把多少张孤立照片当作离群点丢掉"。
- 代价：引入了第二个超参（z），且 3-近似是一般度量的结论，一维下的精确复杂度我**没有找到**对应文献。
- 未经一手核实：多处二手来源称优于 3 的近似是 NP-hard，但我没能在一手文献里核到这句话，此处不作为已确立结论。

**(2) 平均型目标天然弱化离群点。** facility location 的定义是 `f(X) = Σ_{y∈Y} max_{x∈X} φ(x,y)`（[apricot，arXiv:1906.03543](https://ar5iv.labs.arxiv.org/html/1906.03543)）。它是单调 submodular，因此贪心有 Nemhauser-Wolsey-Fisher 的 `(1 - 1/e)` 保证（[Mathematical Programming 14:265-294, 1978](https://link.springer.com/article/10.1007/BF01588971)；apricot 复述为「a greedy algorithm can find a subset whose objective value is guaranteed to be within a constant factor (1−e^{−1}) of the optimal subset」），且该常数在标准复杂度假设下是紧的。最大化它「tend[s] to choose examples that represent the space of the data well」（[Wei、Iyer、Bilmes, ICML 2015](https://proceedings.mlr.press/v37/wei15.pdf)）。

结构上的推论（**由定义直接推出，非某篇论文的原话**）：在 facility location 里一个孤立点只贡献求和式中它自己那一项，权重约 1/|Y|；而在 max-min 里它可以单独决定整个目标值。所以"代表性"目标对离群点的鲁棒性来自目标形状，不需要额外参数。反面是它追求的是**代表**而非**散开**，会倾向于把名额分给稠密时段。

**(3) 质量 + 多样性的可加合成，带近似保证。** Borodin、Lee、Ye 研究「(a) 子集满足某约束；(b) 子集质量高；(c) 子集在距离度量下多样」的联合目标，质量由单调 submodular 函数给出，多样性取被选点两两距离之和（[arXiv:1203.6397](https://arxiv.org/abs/1203.6397)，PODS'12；期刊版 ACM TALG 2017）。

结论（ar5iv 全文）：

> Theorem 1: There is a simple linear time greedy algorithm that achieves a 2-approximation for the max-sum diversification problem with monotone submodular set functions satisfying a cardinality constraint.

> Theorem 2: The local search algorithm achieves an approximation ratio of 2 for max-sum diversification with a matroid constraint.

贪心的打分函数是 `φ'_u(S) = f'_u(S) + λ·d_u(S)`，其中 `f'_u` 是质量边际增益的一半，`λ` 是显式的质量-多样性权重（[ar5iv:1203.6397](https://ar5iv.labs.arxiv.org/html/1203.6397)）。后续工作把三角不等式放宽到 relaxed triangle inequality 并给出对应的比率退化（[arXiv:1511.02402](https://arxiv.org/abs/1511.02402)）。

**对本项目的直接限制**：这个框架需要一个质量项 `f`。按 PRD 决策十，多样性预筛跑在评估之前，那一刻**没有任何质量或内容信号**。因此该框架在预筛这一步不可用，除非引入一个不依赖 AI 的代理质量信号（PRD 未讨论此选项）。

**(4) DPP：乘性的 quality × diversity。** DPP 是「computationally efficient probabilistic models of diverse sets」，可用于「finding diverse sets of high-quality search results」（Kulesza 与 Taskar，[arXiv:1207.6083](https://arxiv.org/abs/1207.6083)，*Foundations and Trends in ML* 5(2-3):123-286）。取样、边缘化、条件化都有精确高效算法，但**选最可能的那个集合（MAP）是 NP-hard**：

> Finding the most likely configuration (MAP) is NP-hard, so approximate inference is necessary. ... greedy algorithms have been used in the past with some empirical success; however, these methods only give approximation guarantees in the special case of DPPs with monotone kernels.

该文给出「a 1/4-approximation guarantee for a general class of non-monotone DPPs」（Gillenwater、Kulesza、Taskar，[NeurIPS 2012](https://proceedings.neurips.cc/paper/2012/hash/6c8dba7d0df1c4a79dd07646be9a26c8-Abstract.html)）。工程上常用的快速贪心 MAP 见 [arXiv:1709.05135](https://arxiv.org/abs/1709.05135)（该文摘要确认「the maximum a posteriori (MAP) inference for DPP ... is NP-hard」）。

需要标注：DPP 常用的核分解 `L = Diag(q) · S · Diag(q)`（q 为逐项质量、S 为相似度）我在本次调研中**未能从一手 PDF 里取到原文引述**（arXiv PDF 无法被抓取工具解码），因此这一分解形式在本笔记中标为**未经一手核实**。NP-hard 与 1/4-近似两条已从 NeurIPS 一手摘要核实。

**(5) MMR：最简单的显式旋钮。** Carbonell 与 Goldstein 的 Maximal Marginal Relevance 用一个 λ 在"与查询相关"和"与已选结果不相似"之间线性插值，是这一族里最早也最简单的形式（SIGIR 1998；[作者主页 PDF](https://www.cs.cmu.edu/~jgc/publication/The_Use_MMR_Diversity_Based_LTMIR_1998.pdf)，[SIGIR'98 摘要页](https://people.eng.unimelb.edu.au/ammoffat/sigir98/abstracts/carbonell.html)）。它**没有近似保证**，是启发式。与 (3) 的关系是：MMR 的相似度项取 max 而非 sum，故不落在 Borodin 等的可证框架内。

### 2.5 Topic 1 小结表

| 方法 | 目标形状 | 一般度量下的保证 | 一维情形 | 离群点行为 |
|---|---|---|---|---|
| 贪心 farthest-point（现状） | max-min 极值 | 1/2-近似（=2-近似比），且 <2 为 NP-hard | 非必需：精确解 `O(n log n + kn)` 已知 | 由构造先吃两端点；经验上偏好噪声点 |
| k-center | 覆盖极值 | 同一贪心，2-近似 | d=1 多项式，d≥2 NP-hard | 同上 |
| k-center/k-median with outliers | 极值 + 丢弃 z 点 | 3-近似（Charikar 2001） | 未查到一维专门结论 | 显式旋钮 z |
| facility location | 平均型（求和取 max） | `(1-1/e)`，紧 | 未查到一维专门结论 | 单点权重约 1/\|Y\|，结构性弱化 |
| max-sum diversification + submodular 质量 | 质量 + λ·距离和 | 2-近似（基数与 matroid 约束皆是） | 未查到一维专门结论 | λ 是显式旋钮，但需要质量项 |
| DPP MAP | quality × diversity 行列式 | NP-hard；非单调核 1/4-近似 | 未查到一维专门结论 | 概率性排斥，非显式旋钮 |
| MMR | 相关性 - λ·最大相似度 | 无保证 | 不适用 | λ 是显式旋钮 |
| 1D k-means / k-medians | 平方/绝对误差和 | 一维精确 | `O(kn²)` DP 及改进 | 平均型，对离群点比 max-min 稳健 |

---

## 三、Topic 2：VLM 与本地小模型的可依赖边界

### 3.1 从文字描述而非像素生成下游文本（服务风险二）

**这个两段式模式有名字，也有成体系的工作。**

- **Socratic Models**（Zeng 等，[arXiv:2204.00598](https://arxiv.org/abs/2204.00598)）把「language is an intermediate representation by which these models can communicate with each other」当作核心主张，用语言把 VLM 与 LM 串起来，不做任何联合训练。
- **PICa**（Yang 等，AAAI 2022，[arXiv:2109.05014](https://arxiv.org/abs/2109.05014)）先「converts the image into captions (or tags) that GPT-3 can understand」，再让纯文本模型完成下游任务，并明确把「what text formats best describe the image content」当作要研究的变量之一。
- **LENS**（Berrios 等，[arXiv:2306.16410](https://arxiv.org/abs/2306.16410)）用一组视觉模块产出「highly descriptive」的文字，交给冻结的 LLM 推理。

**丢了什么：一手的表述。**

PICa 的作者自陈：

> the image is abstracted as text. Captions or tags only provide a partial description of the image, and might miss important visual details necessary for question answering.

（[ar5iv:2109.05014](https://ar5iv.labs.arxiv.org/html/2109.05014)）该文并指出这一限制在 VQAv2 上表现为颜色、计数这类细粒度视觉属性问题上的明显劣势。

Socratic Models 的对应表述是「the degree to which visual details are provided in the captions is largely limited by the capabilities of the VLM」，并承认在 image captioning 上「do not perform as well as methods such as ClipCap which are directly finetuned」（[ar5iv:2204.00598](https://ar5iv.labs.arxiv.org/html/2204.00598)）。

**中间描述的具体程度直接决定下游质量：可直接对比的数字。**

PICa 在 OK-VQA 上的 Table 3（Full 设定）：

| 中间文本形式 | OK-VQA 准确率（Full） |
|---|---|
| 只给 tags | 44.6% |
| 只给生成的 caption（VinVL-COCO） | 46.9% |
| caption + tags | 48.0% |
| 人写的 ground-truth caption ×1 | 48.7% |
| 人写的 ground-truth caption ×5 | **53.3%** |

（[ar5iv:2109.05014](https://ar5iv.labs.arxiv.org/html/2109.05014)，Table 3）

下游模型与提示不变，8.7 个百分点的跨度全部来自中间描述写得多具体、多完整。这是本笔记里对 PRD 风险二最直接的一条外部证据。

**反向证据：通用描述可能几乎不带来增益。**

MMMU（Yue 等，[arXiv:2311.16502](https://ar5iv.labs.arxiv.org/html/2311.16502)，Table 2）给纯文本 LLM 补外部图转文工具：

| 模型 | 纯文本 | +OCR | +LLaVA Caption |
|---|---|---|---|
| FLAN-T5-XXL | 32.1% | 34.7% | 34.8% |
| Vicuna-13B | 33.3% | 35.4% | 33.9% |

原文结论：「The application of OCR and captioning technologies does not yield a significant improvement in the performance of text-only LMMs.」

把这两组放在一起，得到的不是"描述有用/没用"的矛盾，而是一个更精确的判据：**增益来自描述里恰好包含下游任务所需的那些事实**。通用 caption 写的是"图里有什么"，MMMU 的题目要的是图表数值与学科推理，两者不重合，所以增益接近零；OK-VQA 要的是"图里有什么"，所以描述越具体增益越大。

**明确的证据空白**：以上全部是 VQA / 学科推理这类**有正确答案**的下游任务。我**没有找到**任何一手工作直接测量"只给文字描述、让模型写社交媒体文案"的质量，也没有找到该场景下描述具体程度与文案质量的量化关系。PRD 风险二把这件事定为需要真机调参，文献没有提供可省掉这一步的证据。

### 3.2 小模型在约 18 条目上做列表选择与排序（服务风险三）

**(a) 18 这个长度落在文献的常用区间内，但这个区间是给大模型标定的。**

IR 领域的 listwise 重排普遍采用 window size 20、step 10 的滑窗（[arXiv:2604.03642](https://arxiv.org/html/2604.03642) 记载 RankZephyr、FIRST 与该文方法均为「window size of 20 and a step size of 10」）。所以 18 条不属于"超长列表"这一类风险。

**(b) 中等规模模型在 listwise 上的结构性失败有明确记载。**

Qin 等（NAACL Findings 2024，[arXiv:2306.17563](https://ar5iv.labs.arxiv.org/html/2306.17563)）把 Sun 等的 listwise 提示放到 FLAN-UL2（20B）上，观察到：

> The model will either just output few documents (e.g., "[1]"), an ordered list based on id (e.g. "[1] > [2] > [3] …"), or text which is not parseable.

该文把 listwise 的失败模式归为四类，并逐条定义：

- **Missing**：「When LLMs only outputs a partial list of the input documents」
- **Rejection**：「LLMs refuse to perform the ranking task and produce irrelevant outputs」
- **Repetition**：「LLMs output the same document more than once」
- **Inconsistency**：「The same list of documents have different output rankings when they are fed in with different order or context」

对本项目的直接含义：PRD 决策十三设计的清洗（剔除越界、去重、保序）正好覆盖 Repetition 与部分 Missing，退化分界覆盖 Rejection。Inconsistency 不被任何清洗覆盖，因为它产出的结果**完全合法**。

注意规模：FLAN-UL2 是 20B。本项目目标是单位数十亿参数的本地 gemma 类模型，比这更小一档。文献里 7B 级别能稳定做 listwise 的例子（RankVicuna，[arXiv:2309.15088](https://arxiv.org/abs/2309.15088)）是**为该任务专门训练过的**模型；其摘要同时强调闭源 API 的「results that are not reproducible and non-deterministic」。我未能从摘要一手核实 RankVicuna 的具体训练方式（蒸馏来源），此处只按"非通用 instruct 模型"处理。

**(c) 位置偏置：量级级别的影响，且有直接数字。**

同一篇（[ar5iv:2306.17563](https://ar5iv.labs.arxiv.org/html/2306.17563)，Table 5）在 TREC-DL2019 上用 gpt-3.5-turbo 做 listwise：

| 输入初始顺序 | NDCG@10 |
|---|---|
| BM25 序 | 65.80 |
| 完全倒序 | 32.77 |

作者的解释是失败时 listwise 方法「fall back to the initial ranking」，因此结果「highly sensitive to input ordering」。

更一般的位置效应由 **Lost in the Middle**（Liu 等，TACL 2024，[arXiv:2307.03172](https://arxiv.org/abs/2307.03172)）给出：

> performance is often highest when relevant information occurs at the beginning or end of the input context, and significantly degrades when models must access relevant information in the middle of long contexts

即首尾都强、中间弱的 U 形，不是单纯的 primacy 或 recency。

**已发表的对策与其代价**：Tang 等的 permutation self-consistency（NAACL 2024，[arXiv:2310.07712](https://arxiv.org/abs/2310.07712)）反复打乱列表顺序多次调用、再聚合出与所有采样距离最近的中心排列，报告提升 GPT-3.5 上 7-18%、LLaMA v2 70B 上 8-16%。代价是调用次数成倍增加。对本项目而言这是"用更多 AI 开销换稳定性"的一条已验证路径，与 PRD 决策十一的 M 旋钮是不同的开销维度。

新近工作（[arXiv:2604.03642](https://arxiv.org/html/2604.03642)）在 Zephyr-β(7B) 上做控制实验，观察到基线方法「falter when the relevant passage appears later in the list」，即相关项越靠后越吃亏；该文自陈其结论只在 window size 20 上验证过。

**(d) 列表长度增长带来的退化。**

- 一次性把全部候选喂进去（full ranking）对零样本模型是**变差**的：「significantly increases the ranking difficulty of LLM, resulting in a performance drop」，在多数数据集与开源模型上成立；只有在监督微调之后 full ranking 才反超滑窗（TREC 约 +4 分，BEIR 约 +2 分）。该文测试的候选数是 N ∈ {20, 40, 60, 80, 100}（[arXiv:2412.14574](https://arxiv.org/html/2412.14574)）。
- 未经一手核实：多处来源称窗口在 20-30 附近饱和、再加大只引入噪声。我没能把这句话核到具体论文的具体表格，因此不作为已确立结论。

**综合到本项目的判据**：18 条目本身不是长度风险区；风险来自"模型规模比文献中 listwise 可用的下限更小"与"输入顺序对结果有量级影响"两条。PRD 风险三所说的"没有数据"是准确的 - 文献没有覆盖单位数十亿参数本地模型在这个具体任务上的退化率。

### 3.3 约束解码保证什么、不保证什么

**保证语法，不保证语义。** JSONSchemaBench（Geng 等，[arXiv:2501.10868](https://arxiv.org/html/2501.10868v3)）的表述：

> constrained decoding should not affect the generated output as it only filters out the invalid tokens. However, things become more complicated due to ambiguity of tokenization and distributional shifts.

以及双向风险：

> Over-constrained grammar engines risk limiting the expressive power of LMs, potentially preventing the generation of valid responses and negatively impacting downstream task performance.

> under-constrained engines cannot guarantee that all responses will be valid, often necessitating additional post-processing or retry logic.

**不同引擎对真实 schema 的实际支持率差异极大**（同文，empirical coverage）：Guidance 在 GlaiveAI 上 0.96、在 Github Hard 上 0.41；Llamacpp 分别为 0.95 与 0.39；Outlines 在 Github Hard 上仅 0.03；OpenAI/Gemini 的托管实现在多数数据集上垫底（0.06-0.86）。对本项目的含义：**"用了约束解码"不等于"schema 一定被执行"**，取决于引擎；Ollama 走的是 llama.cpp 一系。

**一方文档的说法。** Ollama 官方 structured outputs 文档把该特性描述为把响应约束到 JSON schema 定义的格式，提供「more reliability and consistency than JSON mode」，并给出三条使用建议，其中包括**在提示里另外写明「return as JSON」**、用 Pydantic/Zod 定义 schema、把 temperature 设为 0（[ollama.com/blog/structured-outputs](https://ollama.com/blog/structured-outputs)）。"约束了格式仍需在提示里重复要求"这一条本身即说明该机制只覆盖结构层。

这与 PRD 决策四"描述形状在三处被定义（提示词、schema instruction、约束解码 JSON Schema），必须一起改"的判断方向一致，并给它提供了外部依据。

**对任务准确率的影响：两个一手来源结论相反。** 见 §4。

---

## 四、开放问题与来源间的分歧

### 4.1 来源间的分歧

**分歧一：约束解码对任务质量是伤害还是帮助。**

- Tam 等（EMNLP 2024 Industry，[arXiv:2408.02442](https://ar5iv.labs.arxiv.org/html/2408.02442)）报告「a significant decline in LLMs' reasoning abilities under format restrictions」，且「stricter format constraints generally lead to greater performance degradation」。具体数字：GPT-3.5-Turbo 在 Last Letter 上 56.74%（自由文本）→ 25.20%（JSON-mode）；LLaMA-3-8B 70.07% → 28.00%。同时在分类任务上方向相反，Gemini-1.5-Flash 在 DDXPlus 上 41.59% → 60.36%。该文并强调退化「not primarily due to parsing errors」，而是格式约束改变了生成过程本身。
- Geng 等（[arXiv:2501.10868](https://arxiv.org/html/2501.10868v3)）报告约束解码「consistently improves the performance of downstream tasks up to 4%」，Table 8 给出 GSM8K 83.8% vs 80.1%、Last Letters 54.0% vs 50.7%、Shuffle Objects 55.9% vs 52.6%。

两者用的是同名的任务（Last Letters、Shuffle Objects），结论方向相反。可能的解释（**我的推断，不是任一论文的结论**）：Tam 等测的是"被迫把整个回答塞进 JSON 结构，包括推理过程"，Geng 等测的是"推理自由生成、只有最终答案受 schema 约束"，二者对 chain-of-thought 空间的挤压程度不同。本笔记不裁决这个分歧，只标注它存在，且它直接关系到 PRD 决策十三/十五那次"连选带排带文案"的调用要不要整体走约束解码。

**分歧二：文字中介到底损失多少。**

PICa 显示中间描述质量与下游表现强相关（44.6% → 53.3%），MMMU 显示补 caption 几乎无增益（+0.6 到 +2.7 点）。两者不矛盾，但**不能从任一篇外推出"给定描述足够好，纯文本下游就接近端到端"这个结论** - 没有一手来源做过这个对照（同一模型、同一任务、看图 vs 看该图的高质量描述）。

### 4.2 开放问题

1. **一维下贪心 farthest-point 相对精确解的实际差距未知。** 文献给出 2-近似的最坏情况界与一维的精确算法，但没有针对一维实数据（尤其是时间戳这种密度极不均匀的分布）测量贪心的实际次优程度。本项目要不要换算法，缺一条实测依据。
2. **k-center/k-median with outliers 在一维的精确复杂度未查到。** 若要引入"允许丢弃 z 个孤立点"这个旋钮，一维是否同样有廉价精确解，本次调研未找到答案。
3. **没有文献直接研究"仅用时间戳做照片多样性子集选择"。** 全部证据来自一般度量空间的算法文献或高维点云采样，迁移到本项目属外推。
4. **"描述 → 社交媒体文案"这条链路没有已发表的质量测量。** 描述具体程度与文案质量的关系，只能类比 PICa 在 VQA 上的结论。
5. **单位数十亿参数的本地模型在 18 条目 listwise 选择+排序上的退化率没有已发表数据。** 最接近的证据是 FLAN-UL2（20B）在 listwise 上失败，规模比目标模型大一档。PRD 风险三要求真机测量退化率，文献不能替代。
6. **位置偏置在"选 9 排 9"这种任务上的表现未知。** 现有测量全部针对 IR 重排（有 ground-truth 相关性标签、有初始 BM25 序）。本项目的输入没有有意义的初始序，也没有 ground truth，Qin 等那条"失败时回落到初始排序"的机制在此如何表现，无对应研究。
7. **permutation self-consistency 的性价比未在小模型上验证。** 报告的 7-18% 提升来自 GPT-3.5 与 LLaMA v2 70B，且以成倍调用为代价，与本项目 G4"AI 开销有上界"直接冲突，是否值得未知。
