このアプリは親を対象にしたGmaiと会話するアプリ。

Google AI studioの無料APIをmain.exe(C++)で取り込む。

Gmaiには最低限のキャラ付けと情報のチューニングを施す。
方法はシステムインストラクション（System Instruction）とコンテキスト（背景情報）の埋め込みを使う。

<本開発フォルダ構成>
📂 プロジェクトフォルダ（プロジェクトのルートフォルダ）
├── 📄 .gitignore     　　　　← 【重要】GitHubに無視させるファイルのリスト
├── 📄 .env                   ← 【秘密】
├── 📄 .env.example           ← 【公開用】
├── 📄 README.md              ← アプリの説明書
├── 📂 src/                   ← C++のソースコードを入れるフォルダ
│    └── 📄 main.cpp          ← あなたが書くC++のコード
└── 📂 ui/                    ← 画面の素材フォルダ（.exeが読み込む）
     ├── 📄 index.html        ← LINE風の画面
     ├── 📄 style.css         ← LINE風のデザイン
     └── 📄 app.js            ← ボタンを押した時のJS処理


<完成図>
📂 Gmail_App（完成版）
├── 📄 main.exe               ← これをダブルクリックするとアプリが起動する！
├── 📄 .env                   ← AIキーや親の情報（exeが裏で読み込む）
└── 📂 ui/                    ← 画面の見た目データ（exeが読み込む）
     ├── 📄 index.html        ← LINE風の画面
     ├── 📄 style.css
     └── 📄 app.js