/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Fired when any context ui is displayed
const kContextUIShowEvent = "MozContextUIShow";
// Fired when any context ui is dismissed
const kContextUIDismissEvent = "MozContextUIDismiss";
// Fired when the tabtray is displayed
const kContextUITabsShowEvent = "MozContextUITabsShow";
// add more as needed...

// delay for ContextUI's dismissWithDelay
const kHideContextAndTrayDelayMsec = 3000;

// delay when showing the tab bar briefly as a new tab opens
const kNewTabAnimationDelayMsec = 1000;

/*
 * Manages context UI (navbar, tabbar, appbar) and track visibility. Also
 * tracks events that summon and hide the context UI.
 */
var ContextUI = {
  _expandable: true,
  _hidingId: 0,

  /*******************************************
   * init
   */

  init: function init() {
    Elements.browsers.addEventListener("mousedown", this, true);
    Elements.browsers.addEventListener("touchstart", this, true);
    Elements.browsers.addEventListener("AlertActive", this, true);

    Elements.browsers.addEventListener('URLChanged', this, true);
    Elements.tabList.addEventListener('TabSelect', this, true);
    Elements.panelUI.addEventListener('ToolPanelShown', this, false);
    Elements.panelUI.addEventListener('ToolPanelHidden', this, false);

    window.addEventListener("MozEdgeUIStarted", this, true);
    window.addEventListener("MozEdgeUICanceled", this, true);
    window.addEventListener("MozEdgeUICompleted", this, true);
    window.addEventListener("keypress", this, true);
    window.addEventListener("KeyboardChanged", this, false);

    Elements.tray.addEventListener("transitionend", this, true);

    Appbar.init();
  },

  /*******************************************
   * Context UI state getters & setters
   */

  // any visiblilty
  get isVisible() {
    return this.navbarVisible || this.tabbarVisible || this.contextAppbarVisible;
  },

  // navbar visiblilty
  get navbarVisible() {
    return (Elements.navbar.hasAttribute("visible") ||
            Elements.navbar.hasAttribute("startpage"));
  },

  // tabbar visiblilty
  get tabbarVisible() {
    return Elements.tray.hasAttribute("expanded");
  },

  // appbar visiblilty
  get contextAppbarVisible() {
    return Elements.contextappbar.isShowing;
  },

  // currently not in use, for the always show tabs feature
  get isExpandable() { return this._expandable; },
  set isExpandable(aFlag) {
    this._expandable = aFlag;
    if (!this._expandable)
      this.dismiss();
  },

  /*******************************************
   * Public api
   */

  /*
   * Toggle the current nav UI state. Fires context ui events.
   */
  toggleNavUI: function () {
    // The navbar is forced open when the start page is visible. appbar.js
    // controls the "visible" property, and browser-ui controls the "startpage"
    // property. Hence we rely on the tabbar for current toggle state.
    if (this.tabbarVisible) {
      this.dismiss();
    } else {
      this.displayNavUI();
    }
  },

  /*
   * Show the nav and tabs bar. Returns true if any non-visible UI
   * was shown. Fires context ui events.
   */
  displayNavUI: function () {
    let shown = false;

    if (!this.navbarVisible) {
      this.displayNavbar();
      shown = true;
    }

    if (!this.tabbarVisible) {
      this.displayTabs();
      shown = true;
    }

    if (shown) {
      ContentAreaObserver.update(window.innerWidth, window.innerHeight);
      this._fire(kContextUIShowEvent);
    }

    return shown;
  },

  /*
   * Dismiss any context UI currently visible. Returns true if any
   * visible UI was dismissed. Fires context ui events.
   */
  dismiss: function () {
    let dismissed = false;

    this._clearDelayedTimeout();

    // No assurances this will hide the nav bar. It may have the
    // 'startpage' property set. This removes the 'visible' property.
    if (this.navbarVisible) {
      this.dismissNavbar();
      dismissed = true;
    }
    if (this.tabbarVisible) {
      this.dismissTabs();
      dismissed = true;
    }
    if (Elements.contextappbar.isShowing) {
      this.dismissContextAppbar();
      dismissed = true;
    }

    if (dismissed) {
      ContentAreaObserver.update(window.innerWidth, window.innerHeight);
      this._fire(kContextUIDismissEvent);
    }

    return dismissed;
  },

  /*
   * Briefly show the tab bar and then hide it. Fires context ui events.
   */
  peekTabs: function peekTabs() {
    if (this.tabbarVisible) {
      setTimeout(function () {
        ContextUI.dismissWithDelay(kNewTabAnimationDelayMsec);
      }, 0);
    } else {
      BrowserUI.setOnTabAnimationEnd(function () {
        ContextUI.dismissWithDelay(kNewTabAnimationDelayMsec);
      });
      this.displayTabs();
    }
  },


  /*
   * Dismiss all context ui after a delay. Fires context ui events.
   */
  dismissWithDelay: function dismissWithDelay(aDelay) {
    aDelay = aDelay || kHideContextAndTrayDelayMsec;
    this._clearDelayedTimeout();
    this._hidingId = setTimeout(function () {
        ContextUI.dismiss();
      }, aDelay);
  },

  // Cancel any pending delayed dismiss
  cancelDismiss: function cancelDismiss() {
    this._clearDelayedTimeout();
  },

  // Display the nav bar
  displayNavbar: function () {
    this._clearDelayedTimeout();
    Elements.navbar.show();
  },

  // Display the tab tray
  displayTabs: function () {
    this._clearDelayedTimeout();
    this._setIsExpanded(true);
  },

  // Display the app bar
  displayContextAppbar: function () {
    this._clearDelayedTimeout();
    Elements.contextappbar.show();
  },

  // Dismiss the navbar if visible.
  dismissNavbar: function dismissNavbar() {
    Elements.navbar.dismiss();
  },

  // Dismiss the tabstray if visible.
  dismissTabs: function dimissTabs() {
    this._clearDelayedTimeout();
    this._setIsExpanded(false);
  },

  // Dismiss the appbar if visible.
  dismissContextAppbar: function dismissContextAppbar() {
    Elements.contextappbar.dismiss();
  },

  /*******************************************
   * Internal utils
   */

  // tabtray state
  _setIsExpanded: function _setIsExpanded(aFlag, setSilently) {
    // if the tray can't be expanded, don't expand it.
    if (!this.isExpandable || this.tabbarVisible == aFlag)
      return;

    if (aFlag)
      Elements.tray.setAttribute("expanded", "true");
    else
      Elements.tray.removeAttribute("expanded");

    if (!setSilently)
      this._fire(kContextUITabsShowEvent);
  },

  _clearDelayedTimeout: function _clearDelayedTimeout() {
    if (this._hidingId) {
      clearTimeout(this._hidingId);
      this._hidingId = 0;
    }
  },

  /*******************************************
   * Events
   */

  _onEdgeUIStarted: function(aEvent) {
    this._hasEdgeSwipeStarted = true;
    this._clearDelayedTimeout();

    if (StartUI.hide()) {
      this.dismiss();
      return;
    }
    this.toggleNavUI();
  },

  _onEdgeUICanceled: function(aEvent) {
    this._hasEdgeSwipeStarted = false;
    StartUI.hide();
    this.dismiss();
  },

  _onEdgeUICompleted: function(aEvent) {
    if (this._hasEdgeSwipeStarted) {
      this._hasEdgeSwipeStarted = false;
      return;
    }

    this._clearDelayedTimeout();
    if (StartUI.hide()) {
      this.dismiss();
      return;
    }
    this.toggleNavUI();
  },

  handleEvent: function handleEvent(aEvent) {
    switch (aEvent.type) {
      case "MozEdgeUIStarted":
        this._onEdgeUIStarted(aEvent);
        break;
      case "MozEdgeUICanceled":
        this._onEdgeUICanceled(aEvent);
        break;
      case "MozEdgeUICompleted":
        this._onEdgeUICompleted(aEvent);
        break;
      case "keypress":
        if (String.fromCharCode(aEvent.which) == "z" &&
            aEvent.getModifierState("Win"))
          this.toggleNavUI();
        break;
      case "transitionend":
        setTimeout(function () {
          ContentAreaObserver.updateContentArea();
        }, 0);
        break;
      case "KeyboardChanged":
        this.dismissTabs();
        break;
      case "mousedown":
        if (aEvent.button == 0 && this.isVisible)
          this.dismiss();
        break;
      case 'URLChanged':
        this.dismissTabs();
        break;
      case 'TabSelect':
      case 'ToolPanelShown':
      case 'ToolPanelHidden':
      case "touchstart":
      case "AlertActive":
        this.dismiss();
        break;
    }
  },

  _fire: function (name) {
    let event = document.createEvent("Events");
    event.initEvent(name, true, true);
    window.dispatchEvent(event);
  }
};