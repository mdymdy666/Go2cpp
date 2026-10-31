// 页面只做两件事：轮询 /api/stats，订阅 /events 展示服务端推流。
const statsBox = document.getElementById('stats');
const list = document.getElementById('events');

function appendEvent(text) {
  const item = document.createElement('li');
  item.textContent = text;
  list.prepend(item);
  while (list.children.length > 20) {
    list.lastChild.remove();
  }
}

async function refreshStats() {
  try {
    const response = await fetch('/api/stats');
    statsBox.textContent = JSON.stringify(await response.json(), null, 2);
  } catch (error) {
    statsBox.textContent = '统计接口不可用: ' + error;
  }
}

refreshStats();
setInterval(refreshStats, 3000);

const source = new EventSource('/events');
source.addEventListener('welcome', event => appendEvent('welcome: ' + event.data));
source.addEventListener('tick', event => appendEvent('tick: ' + event.data));
source.addEventListener('message', event => appendEvent('message: ' + event.data));
source.onerror = () => appendEvent('SSE 连接中断（浏览器会自动重连）');

document.getElementById('broadcast').addEventListener('click', async () => {
  try {
    const response = await fetch('/api/broadcast', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ event: 'demo', data: 'hello from browser' }),
    });
    appendEvent('broadcast: ' + (await response.text()));
  } catch (error) {
    appendEvent('广播失败: ' + error);
  }
});
