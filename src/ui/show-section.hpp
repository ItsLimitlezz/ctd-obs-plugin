/* The "Show Profiles" section of the CTD Relay dock (show profiles design sections 2, 4, 6; ticket T12). It only calls
 * the show_* API (src/show/show.h) and is polled by the dock's own timer through refresh(). It hides itself on stock
 * OBS (no CTD OBS core procs), so the rest of the dock is unchanged there. */
#ifndef CTD_UI_SHOW_SECTION_HPP
#define CTD_UI_SHOW_SECTION_HPP

#include <QGroupBox>
#include <QString>

#include <cstdint>

class QLabel;
class QListWidget;
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

	QLabel *status_;
	QListWidget *list_;
	QPushButton *switch_;
	QPushButton *save_;
	QPushButton *update_;
	QPushButton *delete_;
	uint64_t lastSeq_ = UINT64_MAX;
	/* what the buttons depend on, from the last refresh */
	int busy_ = 0;
	bool live_ = false;
	QString liveReason_;
	QString activeId_;
};

#endif
