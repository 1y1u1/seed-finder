#include "presetstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QJsonObject condToJson(const sf::WaveCondition &c)
{
    QJsonObject o;
    o["need"] = double(c.needMask);
    o["refuse"] = double(c.refuseMask);
    o["atLeastMask"] = double(c.atLeastMask);
    o["atMostMask"] = double(c.atMostMask);
    o["atLeast"] = c.atLeast;
    o["atMost"] = c.atMost;
    return o;
}

sf::WaveCondition condFromJson(const QJsonObject &o)
{
    sf::WaveCondition c;
    c.needMask = quint32(o.value("need").toDouble());
    c.refuseMask = quint32(o.value("refuse").toDouble());
    c.atLeastMask = quint32(o.value("atLeastMask").toDouble());
    c.atMostMask = quint32(o.value("atMostMask").toDouble());
    c.atLeast = o.value("atLeast").toInt();
    c.atMost = o.value("atMost").toInt();
    c.sanitize();
    return c;
}

QString appDataPresetPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        // 2.0 用 .seedfinder2; 老版本留下的 .seedfinder3 如果存在就继续用它,
        // 免得升级之后用户以为预设全丢了。
        const QString oldDir = QDir::homePath() + QStringLiteral("/.seedfinder3");
        const QString newDir = QDir::homePath() + QStringLiteral("/.seedfinder2");
        base = QFileInfo(oldDir + QStringLiteral("/presets.json")).exists() ? oldDir : newDir;
    }
    return base + QStringLiteral("/presets.json");
}

QString exeDirPresetPath()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (exeDir.isEmpty()) return QString();
    return QDir(exeDir).filePath(QStringLiteral("presets.json"));
}

// 预设文件路径覆盖: 2.0 的环境变量是 SEEDFINDER2_PRESETS。
// 老脚本里写的 SEEDFINDER3_PRESETS 仍然认, 这样升级不会把自动化弄坏。
QString presetPathOverride()
{
    const QString v2 = qEnvironmentVariable("SEEDFINDER2_PRESETS");
    if (!v2.isEmpty()) return v2;
    return qEnvironmentVariable("SEEDFINDER3_PRESETS");
}

// 读取时的候选路径: exe 目录的 presets.json 只要"存在"就优先用, 不管它是否可写。
// 之前读写共用同一个"可写才用"的判定, 于是把 presets.json 设成只读后,
// 读取会跳到 AppData, 用户已有的预设看起来就凭空消失了。
QStringList presetReadPaths()
{
    QStringList paths;
    const QString override_ = presetPathOverride();
    if (!override_.isEmpty()) {
        paths << override_;
        return paths;
    }
    const QString exePath = exeDirPresetPath();
    if (!exePath.isEmpty()) paths << exePath;
    const QString appPath = appDataPresetPath();
    if (!paths.contains(appPath)) paths << appPath;
    return paths;
}

} // namespace

QString presetFilePath()
{
    // 自检 / 自动化可以用环境变量把预设文件指到临时目录,
    // 否则 --presettest 会往用户真实的 presets.json 里塞一条"自检预设"。
    const QString override_ = presetPathOverride();
    if (!override_.isEmpty()) return override_;

    // 优先放在 exe 旁边, 做成绿色便携版; 目录不可写时才退到 AppData
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (!exeDir.isEmpty()) {
        QDir d(exeDir);
        const QFileInfo probe(d.filePath(QStringLiteral("presets.json")));
        if (d.exists() && (probe.exists() ? probe.isWritable() : QFileInfo(exeDir).isWritable()))
            return probe.absoluteFilePath();
    }
    return appDataPresetPath();
}

QVector<Preset> loadPresets(QString *error)
{
    QVector<Preset> out;

    QString path;
    const QStringList candidates = presetReadPaths();
    for (const QString &c : candidates) {
        if (QFile::exists(c)) {
            path = c;
            break;
        }
    }
    if (path.isEmpty()) return out;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QString::fromUtf8("无法读取预设文件：%1").arg(f.errorString());
        return out;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isArray()) {
        if (error) *error = QString::fromUtf8("预设文件格式不正确。");
        return out;
    }

    const QJsonArray arr = doc.array();
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject o = arr.at(i).toObject();
        Preset p;
        p.name = o.value("name").toString();
        p.uid = o.value("uid").toInt(1);
        p.mode = o.value("mode").toInt(13);
        p.scene = o.value("scene").toInt(2);
        p.beginFlag = o.value("beginFlag").toInt(2);
        p.endFlag = o.value("endFlag").toInt(50);
        p.outputSize = o.value("outputSize").toInt(20);

        const QJsonArray bands = o.value("bands").toArray();
        for (int k = 0; k < bands.size(); ++k) {
            const QJsonObject bo = bands.at(k).toObject();
            sf::RuleBand b;
            b.beginFlag = bo.value("begin").toInt();
            b.endFlag = bo.value("end").toInt();
            b.cond = condFromJson(bo.value("cond").toObject());
            // 空条件的带子等于"不限制", 存进去会污染 effectiveRange 算出的范围,
            // 硬性条件那边一直有这个判断, 排名条件漏了, 这里补上。
            if (b.valid() && !b.cond.isEmpty()) p.bands << b;
        }

        const QJsonArray hard = o.value("hard").toArray();
        for (int k = 0; k < hard.size(); ++k) {
            const QJsonObject ho = hard.at(k).toObject();
            sf::RuleBand h;
            h.beginFlag = ho.value("begin").toInt();
            h.endFlag = ho.value("end").toInt();
            h.cond = condFromJson(ho.value("cond").toObject());
            if (h.valid() && !h.cond.isEmpty()) p.hard << h;
        }

        if (p.name.isEmpty()) p.name = QString::fromUtf8("未命名预设 %1").arg(i + 1);
        out << p;
    }
    return out;
}

bool savePresets(const QVector<Preset> &list, QString *error)
{
    QJsonArray arr;
    for (int i = 0; i < list.size(); ++i) {
        const Preset &p = list[i];
        QJsonObject o;
        o["name"] = p.name;
        o["uid"] = p.uid;
        o["mode"] = p.mode;
        o["scene"] = p.scene;
        o["beginFlag"] = p.beginFlag;
        o["endFlag"] = p.endFlag;
        o["outputSize"] = p.outputSize;

        QJsonArray bands;
        for (int k = 0; k < p.bands.size(); ++k) {
            QJsonObject bo;
            bo["begin"] = p.bands[k].beginFlag;
            bo["end"] = p.bands[k].endFlag;
            bo["cond"] = condToJson(p.bands[k].cond);
            bands.append(bo);
        }
        o["bands"] = bands;

        QJsonArray hard;
        for (int k = 0; k < p.hard.size(); ++k) {
            QJsonObject ho;
            ho["begin"] = p.hard[k].beginFlag;
            ho["end"] = p.hard[k].endFlag;
            ho["cond"] = condToJson(p.hard[k].cond);
            hard.append(ho);
        }
        o["hard"] = hard;

        arr.append(o);
    }

    const QString path = presetFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QString::fromUtf8("无法写入预设文件：%1").arg(f.errorString());
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = QString::fromUtf8("保存预设失败：%1").arg(f.errorString());
        return false;
    }
    return true;
}
