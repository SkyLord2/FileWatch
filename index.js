const path = require('path');

const addonPath = path.join(__dirname, 'FileWatch.node'); // 或 './build/Release/FileWatch' 视编译位置而定
let FileWatch;
try {
    FileWatch = require(addonPath);
} catch (err) {
    console.error('加载本地模块失败:', err);
    process.exit(1);
}

if (typeof FileWatch.WatchInitialize !== 'function') {
    throw new Error('FileWatch.WatchInitialize 未导出或不是函数');
}

// 同步调用（如果 WatchInitialize 返回值）
try {
    const result = FileWatch.WatchInitialize(
        [".txt", ".doc", ".docx", ".pdf", ".xls", ".xlsx", ".ppt", ".pptx"],
        ["C:\\Users\\chengdongsheng\\Downloads", "C:\\Users\\chengdongsheng\\Desktop"],
        (type, path) => { 
            console.log("report: type = " + type);
            console.log("report: path = " + path);
         },
        (log) => { 
            console.log(log);
        }
    );
    console.log('WatchInitialize 返回：', result);
} catch (err) {
    console.error('调用 WatchInitialize 出错：', err);
}

// 或者：异步回调形式（如果实现为接受回调）
if (FileWatch.WatchInitialize.length >= 1) { // 简单检测是否可能接受回调
    FileWatch.WatchInitialize((err, info) => {
        if (err) return console.error('WatchInitialize 回调错误：', err);
        console.log('WatchInitialize 回调结果：', info);
    });
}