import { DashboardPageHelper } from '../ui/dashboard.po';
import { MultiClusterPageHelper } from './multi-cluster.po';

describe('Muti-cluster management page', () => {
  const multiCluster = new MultiClusterPageHelper();
  const dashboard = new DashboardPageHelper();

  const hubName = 'local-cluster';
  const url = Cypress.env('CEPH2_URL');
  const alias = 'ceph2';
  const username = 'admin';
  const password = 'admin';

  const editedAlias = 'ceph2-edited';

  beforeEach(() => {
    cy.login();
    multiCluster.navigateTo('manage-clusters');
  });

  it('should authenticate the second cluster', () => {
    multiCluster.auth(url, alias, username, password);
    multiCluster.existTableCell(alias);
    multiCluster.checkConnectionStatus(alias, 'CONNECTED');
  });

  it('should switch to the second cluster and back to hub', () => {
    multiCluster.checkConnectionStatus(alias, 'CONNECTED');
    dashboard.navigateTo();

    // Switch to second cluster
    cy.get('[data-testid="selected-cluster"]').click();
    cy.get('[data-testid="select-a-cluster"]').contains(alias).click();

    // Ensure that the page has switched to second cluster
    cy.get('[data-testid="selected-cluster"]').contains(alias);

    // Check if login is required (unauthenticated second cluster)
    cy.location('pathname', { timeout: 10000 }).then((path) => {
      if (path.includes('/login')) {
        cy.get('[name=username]').type(username);
        cy.get('#password').type(password);
        cy.get('[type=submit]').click();
      }
    });

    // Confirm dashboard loads after login
    cy.get('cd-dashboard-v3', { timeout: 10000 }).should('exist');

    // Switch back to the hub cluster
    cy.get('[data-testid="selected-cluster"]').click();
    cy.get('[data-testid="select-a-cluster"]').contains(hubName).click();
    cy.get('[data-testid="selected-cluster"]').contains(hubName);
    cy.get('cd-dashboard-v3').should('exist');
  });

  it('should reconnect the second cluster', () => {
    multiCluster.checkConnectionStatus(alias, 'CONNECTED');
    multiCluster.reconnect(alias, password);
    multiCluster.existTableCell(alias);
  });

  it('should edit the second cluster', () => {
    multiCluster.checkConnectionStatus(alias, 'CONNECTED');
    multiCluster.edit(alias, editedAlias);
    multiCluster.existTableCell(editedAlias);
  });

  it('should disconnect the second cluster', () => {
    multiCluster.checkConnectionStatus(editedAlias, 'CONNECTED');
    multiCluster.disconnect(editedAlias);
    multiCluster.existTableCell(editedAlias, false);
  });
});
