/* The "Show Profiles" section of the CTD Relay dock (show profiles design sections 2, 4, 6; ticket T12). It only calls
 * the show_* API (src/show/show.h) and is polled by the dock's own timer through refresh(). It hides itself on stock
 * OBS (no CTD OBS core procs), so the rest of the dock is unchanged there. */
#ifndef CTD_UI_SHOW_SECTION_HPP
#define CTD_UI_SHOW_SECTION_HPP

#include <QGroupBox>
#include <QString>

#include <QHash>

#include <cstdint>

#include "../show/sync.h"

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

class ShowProfilesSection : public QGroupBox {
public:
	explicit ShowProfilesSection(QWidget *parent = nullptr);

	/* Call from the dock's existing 500 ms timer. */
	void refresh();

private:
	QString selectedId() const;
	void updateButtons();
	void askSave();
	void askUpdate();
	void askSwitch();
	void askDelete();
	QString selectedName() const;
	/* M3 (T11): CTD Relay sync */
	void askSaveUp();
	void askPull();
	void askUpdateNow();
	void openDevices();
	void askConflict();
	void showProgress(const show_sync_state &sync);
	void showRemotes(const show_sync_state &sync);

	QLabel *status_;
	QListWidget *list_;
	QPushButton *switch_;
	QPushButton *save_;
	QPushButton *update_;
	QPushButton *delete_;
	QPushButton *saveUp_;
	QPushButton *updateNow_;
	QPushButton *devices_;
	QProgressBar *progress_;
	QLabel *remoteTitle_;
	QListWidget *remote_;
	QPushButton *pull_;
	uint64_t lastSeq_ = UINT64_MAX;
	uint64_t lastSyncSeq_ = UINT64_MAX;
	uint64_t lastConflictSeq_ = 0;
	bool conflictOpen_ = false;
	QHash<QString, show_sync_entry> sync_; /* by show profile id */
	bool relayConnected_ = false;
	QString relayProfileName_;
	/* what the buttons depend on, from the last refresh */
	int busy_ = 0;
	bool live_ = false;
	QString liveReason_;
	QString activeId_;
};

#endif
