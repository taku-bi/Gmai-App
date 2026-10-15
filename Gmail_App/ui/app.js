// ==========================================
//  DOM要素の取得
// ==========================================
const chatMessages = document.getElementById("chat_messages");
const userInput    = document.getElementById("user_input");
const sendBtn      = document.getElementById("send_btn");

// ==========================================
//  メッセージ吹き出しを画面に追加する関数
//  role: "user" または "bot"
// ==========================================
function addMessage(text, role) {
    const messageDiv = document.createElement("div");
    messageDiv.classList.add("message", role);

    const bubbleDiv = document.createElement("div");
    bubbleDiv.classList.add("bubble");
    bubbleDiv.textContent = text;

    messageDiv.appendChild(bubbleDiv);
    chatMessages.appendChild(messageDiv);

    // 最新メッセージが見えるように一番下へスクロール
    chatMessages.scrollTop = chatMessages.scrollHeight;
}

// ==========================================
//  送信処理
// ==========================================
function sendMessage() {
    const text = userInput.value.trim();
    if (text === "") return;

    // 1) ユーザーの吹き出しを表示
    addMessage(text, "user");

    // 2) 入力欄をクリア
    userInput.value = "";

    // 3) C++側（WebView2ホスト）にメッセージを送る
    //    main.exe が WebView2 を使っている前提
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(text);
    }
}

// ==========================================
//  C++側からの返答を受け取る
//  main.exe が webview->PostWebMessageAsString() で送ってくる想定
// ==========================================
if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener("message", function (event) {
        const reply = event.data;
        addMessage(reply, "bot");
    });
}

// ==========================================
//  イベントリスナーの登録
// ==========================================

// 送信ボタンをクリック
sendBtn.addEventListener("click", sendMessage);

// Enterキーでも送信
userInput.addEventListener("keydown", function (event) {
    if (event.key === "Enter") {
        event.preventDefault();
        sendMessage();
    }
});

